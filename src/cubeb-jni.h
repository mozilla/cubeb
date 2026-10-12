#ifndef _CUBEB_JNI_H_
#define _CUBEB_JNI_H_

#include <stdint.h>

typedef struct cubeb_jni cubeb_jni;

#ifdef __cplusplus
extern "C" {
#endif

cubeb_jni *
cubeb_jni_init();
int
cubeb_get_output_latency_from_jni(cubeb_jni * cubeb_jni_ptr);
int
cubeb_get_output_frames_per_buffer_from_jni(cubeb_jni * cubeb_jni_ptr,
                                            uint32_t * frames);
int
cubeb_get_output_sample_rate_from_jni(cubeb_jni * cubeb_jni_ptr,
                                      uint32_t * rate);
void
cubeb_jni_destroy(cubeb_jni * cubeb_jni_ptr);

#ifdef __cplusplus
};
#endif

#endif // _CUBEB_JNI_H_
