/*
 * Copyright © 2017 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 *
 *
 * Purpose for the test
 * =============================================================================
 * In CoreAudio, the data callback will holds a mutex shared with AudioUnit.
 * Thus, if the callback request another mutex M(resource) held by the another
 * function, without releasing its holding one, then it will cause a deadlock
 * when the another function, which holds the mutex M, request to use AudioUnit.
 *
 * We illustrate a simple version of the deadlock in bug 1337805 as follows:
 * https://bugzilla.mozilla.org/show_bug.cgi?id=1337805
 *
 *                   holds
 *  data_callback <---------- mutext_AudioUnit
 *      |                            ^
 *      |                            |
 *      | request                    | request
 *      |                            |
 *      v           holds            |
 *  mutex_cubeb ------------> get_channel_layout
 *
 * In this example, the "audiounit_get_channel_layout" in f4edfb8:
 * https://github.com/kinetiknz/cubeb/blob/f4edfb8eea920887713325e44773f3a2d959860c/src/cubeb_audiounit.cpp#L2725
 * requests to create an AudioUnit when it holds a mutex of cubeb context.
 * Meanwhile, the data callback who holds the mutex shared with AudioUnit
 * requests for the mutex of cubeb context. As a result, it causes a deadlock.
 *
 * The problem is solve by pull 236: https://github.com/kinetiknz/cubeb/pull/236
 * We store the latest channel layout and return it when there is an active
 * AudioUnit, otherwise, we will create an AudioUnit to get it.
 *
 * However, to prevent it happens again, we add the test here in case who
 * without such knowledge misuses the AudioUnit in get_channel_layout.
 */

#include "gtest/gtest.h"
#include "common.h"       // for layout_infos
#include "cubeb/cubeb.h"  // for cubeb utils
#include <iostream>       // for fprintf
#include <mutex>          // for std::mutex
#include <pthread.h>      // for pthread
#include <signal.h>       // for signal
#include <stdexcept>      // for std::logic_error
#include <string>         // for std::string
#include <unistd.h>       // for sleep, usleep

// The signal alias for calling our thread killer.
#define CALL_THREAD_KILLER SIGUSR1

// This indicator will become true when our pending task thread is killed by
// ourselves.
bool killed = false;

// This indicator will become true when the assigned task is done.
bool task_done = false;

// Indicating the data callback is fired or not.
bool called = false;

// Toggle to true when running data callback. Before data callback gets
// the mutex of cubeb context, it toggle back to false.
// The task to get channel layout should be executed when this is true.
bool callbacking_before_getting_context = false;

std::mutex context_mutex;
cubeb * context = nullptr;

cubeb * get_cubeb_context_unlocked()
{
  if (context) {
    return context;
  }

  int r = CUBEB_OK;
  r = cubeb_init(&context, "Cubeb deadlock test", NULL);
  if (r != CUBEB_OK) {
    context = nullptr;
  }

  return context;
}

cubeb * get_cubeb_context()
{
  std::lock_guard<std::mutex> lock(context_mutex);
  return get_cubeb_context_unlocked();
}

struct cubeb_cleaner
{
  cubeb_cleaner(cubeb * context) : ctx(context) {}
  ~cubeb_cleaner() { cubeb_destroy(ctx); }
  cubeb * ctx;
};

struct cubeb_stream_cleaner
{
  cubeb_stream_cleaner(cubeb_stream * stream) : stm(stream) {}
  ~cubeb_stream_cleaner() { cubeb_stream_destroy(stm); }
  cubeb_stream * stm;
};

void state_cb_audio(cubeb_stream * /*stream*/, void * /*user*/, cubeb_state /*state*/)
{
}

// Fired by coreaudio's rendering mechanism. It holds a mutex shared with the
// current used AudioUnit.
template<typename T>
long data_cb(cubeb_stream * /*stream*/, void * /*user*/,
             const void * /*inputbuffer*/, void * /*outputbuffer*/, long nframes)
{
  called = true;

  if (!task_done) {
    callbacking_before_getting_context = true;
    fprintf(stderr, "time to switch thread!\n");
    usleep(100000); // Force to switch threads by sleeping 100 ms.
    callbacking_before_getting_context = false;
  }

  fprintf(stderr, "try getting backend id ...\n");

  // Try requesting mutex of context when holding mutex of AudioUnit.
  char const * backend_id = cubeb_get_backend_id(get_cubeb_context());
  fprintf(stderr, "callback on %s\n", backend_id);

  return nframes;
}

// Called by wait_to_get_layout, which is run out of main thread.
void get_preferred_channel_layout()
{
  std::lock_guard<std::mutex> lock(context_mutex);
  cubeb * context = get_cubeb_context_unlocked();
  ASSERT_TRUE(!!context);

  // We will cause a deadlock if cubeb_get_preferred_channel_layout requests
  // mutext of AudioUnit when it holds mutex of context.
  cubeb_channel_layout layout;
  int r = cubeb_get_preferred_channel_layout(context, &layout);
  ASSERT_EQ(r == CUBEB_OK, layout != CUBEB_LAYOUT_UNDEFINED);
  fprintf(stderr, "layout is %s\n", layout_infos[layout].name);
}

void * wait_to_get_layout(void *)
{
  while(!callbacking_before_getting_context) {
    fprintf(stderr, "waiting for data callback ...\n");
    usleep(1000); // Force to switch threads by sleeping 1 ms.
  }

  fprintf(stderr, "try getting channel layout ...\n");
  get_preferred_channel_layout(); // Deadlock checkpoint.
  task_done = true;

  return NULL;
}

void * watchdog(void * s)
{
  pthread_t subject = *((pthread_t *) s);

  sleep(2); // Sleep 2 seconds before detecting deadlock.

  if (!task_done) {
    pthread_kill(subject, CALL_THREAD_KILLER);
    pthread_detach(subject);
    // pthread_kill doesn't release mutexes held by the killed thread,
    // so we need to unlock them manually.
    context_mutex.unlock();
  }

  fprintf(stderr, "The assigned task is %sdone\n", (task_done) ? "" : "not ");

  return NULL;
}

void thread_killer(int signal)
{
  ASSERT_EQ(signal, CALL_THREAD_KILLER);
  fprintf(stderr, "kill the task thread!\n");
  killed = true;
}

TEST(cubeb, run_deadlock_test)
{
#if !defined(__APPLE__)
  FAIL() << "Deadlock test is only for OSX now";
#endif

  cubeb * ctx = get_cubeb_context();
  ASSERT_TRUE(!!ctx);

  cubeb_cleaner cleanup_cubeb_at_exit(ctx);

  cubeb_stream_params params;
  params.format = CUBEB_SAMPLE_FLOAT32NE;
  params.rate = 44100;
  params.channels = 2;
  params.layout = CUBEB_LAYOUT_STEREO;

  cubeb_stream * stream = NULL;
  int r = cubeb_stream_init(ctx, &stream, "test deadlock", NULL, NULL, NULL, &params,
      4096, &data_cb<float>, state_cb_audio, NULL);
  ASSERT_EQ(r, CUBEB_OK);

  cubeb_stream_cleaner cleanup_stream_at_exit(stream);

  // Install signal handler.
  signal(CALL_THREAD_KILLER, thread_killer);

  pthread_t subject, detector;
  pthread_create(&subject, NULL, wait_to_get_layout, NULL);
  pthread_create(&detector, NULL, watchdog, (void *) &subject);

  cubeb_stream_start(stream);

  pthread_join(subject, NULL);
  pthread_join(detector, NULL);

  ASSERT_TRUE(called);

  fprintf(stderr, "\n%sDeadlock detected!\n", (called && !task_done) ? "" : "No ");

  // Check the task is killed by ourselves if deadlock happends.
  // Otherwise, the thread killer should not be triggered.
  ASSERT_NE(task_done, killed);

  ASSERT_TRUE(task_done);
}
