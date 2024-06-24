#ifndef _CUBEB_JNI_INSTANCES_H_
#define _CUBEB_JNI_INSTANCES_H_

#include "cubeb/cubeb.h"
#include <jni.h>

/*
 * The methods in this file offer a way to pass in the required
 * JNI instances in the cubeb library. By default they return NULL.
 * In this case part of the cubeb API that depends on JNI
 * will return CUBEB_ERROR_NOT_SUPPORTED. Currently these methods depend on
 * that:
 *
 * opensl: cubeb_stream_get_position()
 * aaudio: cubeb_get_supported_input_processing_params()
 *
 * Users that want to use that cubeb API method must "override"
 * the methods below to return a valid instance of JavaVM
 * and application's Context object.
 * */

inline JNIEnv *
cubeb_get_jni_env_for_thread()
{
  return nullptr;
}

inline jobject
cubeb_jni_get_context_instance()
{
  return nullptr;
}

/*
 * The below methods are to be implemented by the client to provide extra
 * functionality that depends on JNI. These are here because the client may be
 * better suited to generate the relevant JNI bits than us doing it manually.
 */

/**
 * Analoguous to AcousticEchoCanceller.isAvailable().
 * This function must be thread-safe.
 */
inline int
cubeb_jni_acoustic_echo_canceller_is_available(bool * available)
{
  return CUBEB_ERROR_NOT_SUPPORTED;
}

#endif //_CUBEB_JNI_INSTANCES_H_
