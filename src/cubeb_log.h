/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef CUBEB_LOG
#define CUBEB_LOG

#include <stdio.h>

extern cubeb_log_level g_log_level;
extern cubeb_log_callback g_log_callback;

#define LOGV(...) do {                               \
  LOG_INTERNAL(CUBEB_LOG_VERBOSE, __VA_ARGS__);      \
} while(0)

#define LOG(...) do {                                \
  LOG_INTERNAL(CUBEB_LOG_NORMAL, __VA_ARGS__);       \
} while(0)

#define LOG_INTERNAL(level, ...) do {                                \
  if (g_log_callback && level >= g_log_level) {                      \
    if ((FILE *)g_log_callback == stderr ||                          \
        (FILE *)g_log_callback == stdout) {                          \
      fprintf((FILE*)g_log_callback, "%s:%d: ", __FILE__, __LINE__); \
      fprintf((FILE*)g_log_callback, __VA_ARGS__);                   \
      fprintf((FILE*)g_log_callback, "\n");                          \
    } else {                                                         \
      const int MAX_MSG_SIZE = 256;                                  \
      char buf[MAX_MSG_SIZE];                                        \
      int fileandline =                                              \
        snprintf(buf, MAX_MSG_SIZE, "%s:%d: ", __FILE__, __LINE__);  \
      int rv = snprintf(buf + strlen(buf),                           \
                        MAX_MSG_SIZE - fileandline,                  \
                        __VA_ARGS__);                                \
      if (rv >= MAX_MSG_SIZE) {                                      \
        fprintf(stderr,                                              \
                "Message truncated (size: %d, max was %d)\n",        \
                rv, MAX_MSG_SIZE);                                   \
      }                                                              \
      ((cubeb_log_callback)g_log_callback)(buf);                     \
    }                                                                \
  }                                                                  \
} while(0)

#endif // CUBEB_LOG
