/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#define NOMINMAX

#include "cubeb_ringbuffer.h"
#include "cubeb_log.h"
#include <cstdarg>

cubeb_log_level g_log_level;
cubeb_log_callback g_log_callback;

/** The maximum size of a log message, after having been formatted. */
const size_t CUBEB_LOG_MESSAGE_MAX_SIZE = 256;
/** The maximum number of log messages that can be queued before dropping
 * messages. */
const size_t CUBEB_LOG_MESSAGE_QUEUE_DEPTH = 40;
/** Number of milliseconds to wait before dequeuing log messages. */
const std::chrono::milliseconds CUBEB_LOG_BATCH_PRINT_INTERVAL_MS =
                                              std::chrono::milliseconds(10);

/**
  * This wraps an inline buffer, that represents a log message, that must be
  * null-terminated.
  * This class should not use system calls or other potentially blocking code.
  */
class cubeb_log_message
{
public:
  cubeb_log_message()
  {
    PodZero(storage, CUBEB_LOG_MESSAGE_MAX_SIZE);
  }
  cubeb_log_message(char str[CUBEB_LOG_MESSAGE_MAX_SIZE])
  {
    size_t length = strlen(str);
    /* paranoia against malformed messagd */
    assert(length < CUBEB_LOG_MESSAGE_MAX_SIZE);
    if (length > CUBEB_LOG_MESSAGE_MAX_SIZE - 1) {
      return;
    }
    PodCopy(storage, str, length);
    PodZero(storage + length, CUBEB_LOG_MESSAGE_MAX_SIZE - length);
  }
  char * get() {
    return storage;
  }
private:
  char storage[CUBEB_LOG_MESSAGE_MAX_SIZE];
};

/** Lock-free asynchronous logger, made so that logging from a
 *  real-time audio callback does not block the audio thread. */
class cubeb_async_logger
{
public:
  /* This is thread-safe since C++11 */
  static cubeb_async_logger& get() {
    static cubeb_async_logger instance;
    return instance;
  }
  void push(char str[CUBEB_LOG_MESSAGE_MAX_SIZE])
  {
    cubeb_log_message msg(str);
    msg_queue.enqueue(msg);
  }
  void run()
  {
    std::thread([this]() {
      while (true) {
        cubeb_log_message msg;
        while (msg_queue.dequeue(&msg, 1)) {
          LOGV("%s", msg.get());
        }
        std::this_thread::sleep_for(CUBEB_LOG_BATCH_PRINT_INTERVAL_MS);
      }
    }).detach();
  }
private:
  cubeb_async_logger()
    :msg_queue(CUBEB_LOG_MESSAGE_QUEUE_DEPTH)
  {
    run();
  }
  /** This is quite a big data structure, but is only instantiated if the
   * asynchronous logger is used.*/
  lock_free_queue<cubeb_log_message> msg_queue;
};


void cubeb_async_log(const char * fmt, ...)
{
  // This is going to copy a 256 bytes array around, which is fine.
  // We don't want to allocate memory here, because this is made to
  // be called from a real-time callback.
  va_list args;
  va_start(args, fmt);
  char msg[CUBEB_LOG_MESSAGE_MAX_SIZE];
  vsnprintf(msg, CUBEB_LOG_MESSAGE_MAX_SIZE, fmt, args);
  cubeb_async_logger::get().push(msg);
  va_end(args);
}
