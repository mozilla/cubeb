//
// Created by Alex Chronopoulos on 6/8/16.
//

#ifndef CUBEB_TEST_ANDROID_DECL_H
#define CUBEB_TEST_ANDROID_DECL_H

#include <android/log.h>

#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_Test" , ## args)
#define fprintf(arg1, args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_Test_fprintf" , ## args)
#define printf(args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_Test_printf" , ## args)

#ifdef __cplusplus
extern "C" {
#endif

int test_audio();
int test_duplex();
int test_latency();
int test_record();
int test_resampler();
int test_sanity();
int test_tone();

#ifdef __cplusplus
} //end extern "C"
#endif

#endif //CUBEB_TEST_ANDROID_DECL_H
