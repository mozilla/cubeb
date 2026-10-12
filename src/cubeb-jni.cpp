/* clang-format off */
#include "cubeb/cubeb.h"
#include "cubeb_log.h"
#include "jni.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include "cubeb-jni-instances.h"
/* clang-format on */

#define AUDIO_STREAM_TYPE_MUSIC 3

struct cubeb_jni {
  jobject s_audio_manager_obj = nullptr;
  jclass s_audio_manager_class = nullptr;
  jmethodID s_get_output_latency_id = nullptr;
  jmethodID s_get_property_id = nullptr;
};

extern "C" cubeb_jni *
cubeb_jni_init()
{
  jobject ctx_obj = cubeb_jni_get_context_instance();
  JNIEnv * jni_env = cubeb_get_jni_env_for_thread();
  if (!jni_env || !ctx_obj) {
    return nullptr;
  }

  cubeb_jni * cubeb_jni_ptr = new cubeb_jni;
  assert(cubeb_jni_ptr);

  // Find the audio manager object and make it global to call it from another
  // method
  jclass context_class = jni_env->FindClass("android/content/Context");
  jfieldID audio_service_field = jni_env->GetStaticFieldID(
      context_class, "AUDIO_SERVICE", "Ljava/lang/String;");
  jstring jstr = (jstring)jni_env->GetStaticObjectField(context_class,
                                                        audio_service_field);
  jmethodID get_system_service_id =
      jni_env->GetMethodID(context_class, "getSystemService",
                           "(Ljava/lang/String;)Ljava/lang/Object;");
  jobject audio_manager_obj =
      jni_env->CallObjectMethod(ctx_obj, get_system_service_id, jstr);
  cubeb_jni_ptr->s_audio_manager_obj =
      reinterpret_cast<jobject>(jni_env->NewGlobalRef(audio_manager_obj));

  // Make the audio manager class a global reference in order to preserve method
  // id
  jclass audio_manager_class = jni_env->FindClass("android/media/AudioManager");
  cubeb_jni_ptr->s_audio_manager_class =
      reinterpret_cast<jclass>(jni_env->NewGlobalRef(audio_manager_class));
  cubeb_jni_ptr->s_get_output_latency_id =
      jni_env->GetMethodID(audio_manager_class, "getOutputLatency", "(I)I");
  cubeb_jni_ptr->s_get_property_id =
      jni_env->GetMethodID(audio_manager_class, "getProperty",
                           "(Ljava/lang/String;)Ljava/lang/String;");

  jni_env->DeleteLocalRef(ctx_obj);
  jni_env->DeleteLocalRef(context_class);
  jni_env->DeleteLocalRef(jstr);
  jni_env->DeleteLocalRef(audio_manager_obj);
  jni_env->DeleteLocalRef(audio_manager_class);

  return cubeb_jni_ptr;
}

extern "C" int
cubeb_get_output_latency_from_jni(cubeb_jni * cubeb_jni_ptr)
{
  assert(cubeb_jni_ptr);
  JNIEnv * jni_env = cubeb_get_jni_env_for_thread();
  return jni_env->CallIntMethod(
      cubeb_jni_ptr->s_audio_manager_obj,
      cubeb_jni_ptr->s_get_output_latency_id,
      AUDIO_STREAM_TYPE_MUSIC); // param: AudioManager.STREAM_MUSIC
}

static bool
clear_pending_exception(JNIEnv * jni_env)
{
  if (!jni_env->ExceptionCheck()) {
    return false;
  }
  jni_env->ExceptionClear();
  return true;
}

static int
get_uint_property_from_jni(cubeb_jni * cubeb_jni_ptr, char const * property,
                           uint32_t * result)
{
  assert(cubeb_jni_ptr && property && result);
  JNIEnv * jni_env = cubeb_get_jni_env_for_thread();

  jstring jproperty = jni_env->NewStringUTF(property);
  if (!jproperty) {
    clear_pending_exception(jni_env);
    LOG("Error: NewStringUTF failed for %s", property);
    return CUBEB_ERROR;
  }

  jstring jresult = (jstring)jni_env->CallObjectMethod(
      cubeb_jni_ptr->s_audio_manager_obj, cubeb_jni_ptr->s_get_property_id,
      jproperty);
  jni_env->DeleteLocalRef(jproperty);
  if (clear_pending_exception(jni_env)) {
    LOG("Error: AudioManager.getProperty threw for %s", property);
    return CUBEB_ERROR;
  }
  if (!jresult) {
    LOG("Error: AudioManager.getProperty returned null for %s", property);
    return CUBEB_ERROR;
  }

  char const * value = jni_env->GetStringUTFChars(jresult, nullptr);
  if (!value) {
    clear_pending_exception(jni_env);
    jni_env->DeleteLocalRef(jresult);
    LOG("Error: GetStringUTFChars failed for %s", property);
    return CUBEB_ERROR;
  }

  errno = 0;
  char * end = nullptr;
  long parsed = strtol(value, &end, 10);
  bool valid = errno == 0 && end != value && *end == '\0' && parsed > 0 &&
               parsed <= INT32_MAX;
  if (!valid) {
    LOG("Error: invalid value \"%s\" for %s", value, property);
  }

  jni_env->ReleaseStringUTFChars(jresult, value);
  jni_env->DeleteLocalRef(jresult);

  if (!valid) {
    return CUBEB_ERROR;
  }

  *result = static_cast<uint32_t>(parsed);
  return CUBEB_OK;
}

extern "C" int
cubeb_get_output_frames_per_buffer_from_jni(cubeb_jni * cubeb_jni_ptr,
                                            uint32_t * frames)
{
  return get_uint_property_from_jni(
      cubeb_jni_ptr, "android.media.property.OUTPUT_FRAMES_PER_BUFFER", frames);
}

extern "C" int
cubeb_get_output_sample_rate_from_jni(cubeb_jni * cubeb_jni_ptr,
                                      uint32_t * rate)
{
  return get_uint_property_from_jni(
      cubeb_jni_ptr, "android.media.property.OUTPUT_SAMPLE_RATE", rate);
}

extern "C" void
cubeb_jni_destroy(cubeb_jni * cubeb_jni_ptr)
{
  assert(cubeb_jni_ptr);

  JNIEnv * jni_env = cubeb_get_jni_env_for_thread();
  assert(jni_env);

  jni_env->DeleteGlobalRef(cubeb_jni_ptr->s_audio_manager_obj);
  jni_env->DeleteGlobalRef(cubeb_jni_ptr->s_audio_manager_class);

  delete cubeb_jni_ptr;
}
