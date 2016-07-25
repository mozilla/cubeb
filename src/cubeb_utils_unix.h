/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#if !defined(CUBEB_UTILS_UNIX)
#define CUBEB_UTILS_UNIX

#include <pthread.h>

/* This wraps a critical section to track the owner in debug mode. */
class owned_critical_section
{
public:
  owned_critical_section()
  {
    int r;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    r = pthread_mutex_init(&mutex, &attr);
    assert(r == 0);
    pthread_mutexattr_destroy(&attr);
  }

  ~owned_critical_section()
  {
    int r = pthread_mutex_destroy(&mutex);
    assert(r == 0);
  }

  void enter()
  {
    pthread_mutex_lock(&mutex);
  }

  void leave()
  {
    pthread_mutex_unlock(&mutex);
  }

  void assert_current_thread_owns()
  {
#ifdef DEBUG
    int r = pthread_mutex_lock(&mutex);
    assert(r == EDEADLK);
#endif
  }

private:
  pthread_mutex_t mutex;
};

#endif /* CUBEB_UTILS_UNIX */
