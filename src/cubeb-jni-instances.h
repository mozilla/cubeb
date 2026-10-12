#ifndef _CUBEB_JNI_INSTANCES_H_
#define _CUBEB_JNI_INSTANCES_H_

/*
 * The methods in this file offer a way to pass in the required
 * JNI instances in the cubeb library. By default they return NULL.
 * In this case part of the cubeb API that depends on JNI
 * will return CUBEB_ERROR_NOT_SUPPORTED. Currently only the
 * following methods depend on that:
 *
 * cubeb_stream_get_position()
 * cubeb_get_min_latency()
 * cubeb_get_preferred_sample_rate()
 *
 * Users that want to use those cubeb API methods must "override"
 * the methods below to return a valid instance of JavaVM
 * and application's Context object.
 * */

JNIEnv *
cubeb_get_jni_env_for_thread()
{
  return nullptr;
}

jobject
cubeb_jni_get_context_instance()
{
  return nullptr;
}

#endif //_CUBEB_JNI_INSTANCES_H_
