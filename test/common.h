/*
 * Copyright © 2013 Sebastien Alaiwan
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#ifndef COMMON_H
#define COMMON_H

#if defined( _WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>

#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_Test" , ## args)
#define fprintf(arg1, args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_Test_fprintf" , ## args)
#define printf(args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_Test_printf" , ## args)

#endif // __ANDROID__

void delay(unsigned int ms)
{
#if defined(_WIN32)
  Sleep(ms);
#else
  sleep(ms / 1000);
  usleep(ms % 1000 * 1000);
#endif
}

#if !defined(M_PI)
#define M_PI 3.14159265358979323846
#endif

int has_available_input_device(cubeb * ctx)
{
  cubeb_device_collection * devices;
  int input_device_available = 0;
  int r;
  /* Bail out early if the host does not have input devices. */
  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_INPUT, &devices);
  if (r != CUBEB_OK) {
    fprintf(stderr, "error enumerating devices.");
    return 0;
  }

  if (devices->count == 0) {
    fprintf(stderr, "no input device available, skipping test.\n");
    return 0;
  }

  for (uint32_t i = 0; i < devices->count; i++) {
    input_device_available |= (devices->device[i]->state ==
                               CUBEB_DEVICE_STATE_ENABLED);
  }

  if (!input_device_available) {
    fprintf(stderr, "there are input devices, but they are not "
        "available, skipping\n");
    return 0;
  }

  return 1;
}

#endif // COMMON_H

