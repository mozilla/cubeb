/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#include "cubeb_android_communication.h"
#include "gtest/gtest.h"
#include <cstring>
#include <string>
#include <thread>
#include <vector>

struct mock_audio_manager {
  JNIEnv env;
  JNINativeInterface functions{};
  _jobject context;
  _jobject manager;
  _jclass object_class;
  _jstring service_name;
  _jobject device_list;
  struct device : _jobject {
    jint type;
    jint id;
    device(jint type, jint id) : type(type), id(id) {}
  };
  std::vector<device> devices;
  std::vector<std::string> events;
  jint sdk = 31;
  bool reject_device = false;
  bool throw_device = false;
  std::vector<jint> modes;
  int global_refs = 0;
  int frames = 0;
  bool exception = false;
  bool fail_lookup = false;
  bool fail_set_mode = false;

  enum method {
    service = 1,
    set_mode,
    available,
    set_device,
    clear_device,
    start_sco,
    stop_sco,
    set_sco,
    size,
    get,
    type,
    id
  };

  static mock_audio_manager & from(JNIEnv * env)
  {
    return *reinterpret_cast<mock_audio_manager *>(env);
  }

  mock_audio_manager()
  {
    env.functions = &functions;
    functions.PushLocalFrame = [](JNIEnv * env, jint) {
      ++from(env).frames;
      return JNI_OK;
    };
    functions.PopLocalFrame = [](JNIEnv * env, jobject object) {
      --from(env).frames;
      return object;
    };
    functions.GetObjectClass = [](JNIEnv * env, jobject) -> jclass {
      return &from(env).object_class;
    };
    functions.FindClass = [](JNIEnv * env, char const *) -> jclass {
      return &from(env).object_class;
    };
    functions.GetStaticFieldID = [](JNIEnv *, jclass, char const *,
                                    char const *) {
      return reinterpret_cast<jfieldID>(1);
    };
    functions.GetStaticIntField = [](JNIEnv * env, jclass, jfieldID) {
      return from(env).sdk;
    };
    functions.GetMethodID = [](JNIEnv * env, jclass, char const * name,
                               char const *) -> jmethodID {
      auto & state = from(env);
      if (state.fail_lookup) {
        state.exception = true;
        return nullptr;
      }
      const char * names[] = {"getSystemService",
                              "setMode",
                              "getAvailableCommunicationDevices",
                              "setCommunicationDevice",
                              "clearCommunicationDevice",
                              "startBluetoothSco",
                              "stopBluetoothSco",
                              "setBluetoothScoOn",
                              "size",
                              "get",
                              "getType",
                              "getId"};
      for (uintptr_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(name, names[i])) {
          return reinterpret_cast<jmethodID>(i + 1);
        }
      }
      ADD_FAILURE() << "Unexpected JNI method: " << name;
      return nullptr;
    };
    functions.NewStringUTF = [](JNIEnv * env, char const * name) -> jstring {
      EXPECT_STREQ(name, "audio");
      return &from(env).service_name;
    };
    functions.CallObjectMethodV = [](JNIEnv * env, jobject, jmethodID method,
                                     va_list args) -> jobject {
      auto & state = from(env);
      switch (reinterpret_cast<uintptr_t>(method)) {
      case service:
        return &state.manager;
      case available:
        return &state.device_list;
      case get:
        return &state.devices.at(va_arg(args, jint));
      default:
        ADD_FAILURE();
        return nullptr;
      }
    };
    functions.CallIntMethodV = [](JNIEnv * env, jobject object,
                                  jmethodID method, va_list) -> jint {
      auto & state = from(env);
      switch (reinterpret_cast<uintptr_t>(method)) {
      case size:
        return state.devices.size();
      case type:
        return static_cast<device *>(object)->type;
      case id:
        return static_cast<device *>(object)->id;
      default:
        ADD_FAILURE();
        return 0;
      }
    };
    functions.CallBooleanMethodV = [](JNIEnv * env, jobject, jmethodID method,
                                      va_list args) -> jboolean {
      auto & state = from(env);
      EXPECT_EQ(reinterpret_cast<uintptr_t>(method), set_device);
      auto * selected = static_cast<device *>(va_arg(args, jobject));
      state.events.push_back("device:" + std::to_string(selected->id));
      state.exception = state.throw_device;
      return !state.reject_device && !state.throw_device;
    };
    functions.DeleteLocalRef = [](JNIEnv *, jobject) {};
    functions.NewGlobalRef = [](JNIEnv * env, jobject object) -> jobject {
      ++from(env).global_refs;
      return object;
    };
    functions.DeleteGlobalRef = [](JNIEnv * env, jobject) {
      --from(env).global_refs;
    };
    functions.CallVoidMethodV = [](JNIEnv * env, jobject, jmethodID method,
                                   va_list args) {
      auto & state = from(env);
      switch (reinterpret_cast<uintptr_t>(method)) {
      case set_mode: {
        jint mode = va_arg(args, jint);
        state.modes.push_back(mode);
        state.events.push_back("mode:" + std::to_string(mode));
        state.exception = state.fail_set_mode;
        break;
      }
      case clear_device:
        state.events.push_back("clear");
        break;
      case start_sco:
        state.events.push_back("start_sco");
        break;
      case stop_sco:
        state.events.push_back("stop_sco");
        break;
      case set_sco:
        state.events.push_back(va_arg(args, jint) ? "sco:on" : "sco:off");
        break;
      default:
        ADD_FAILURE();
      }
    };
    functions.ExceptionCheck = [](JNIEnv * env) -> jboolean {
      return from(env).exception;
    };
    functions.ExceptionClear = [](JNIEnv * env) {
      from(env).exception = false;
    };
  }
};

TEST(android_communication, overlapping_streams)
{
  mock_audio_manager jni;
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  EXPECT_EQ(jni.modes, (std::vector<jint>{3}));
  EXPECT_EQ(jni.global_refs, 1);
  mode.release(&jni.env);
  EXPECT_EQ(jni.modes, (std::vector<jint>{3}));
  mode.release(&jni.env);
  EXPECT_EQ(jni.modes, (std::vector<jint>{3, 0}));
  EXPECT_EQ(jni.global_refs, 0);
  EXPECT_EQ(jni.frames, 0);

  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  mode.release(&jni.env);
  EXPECT_EQ(jni.modes, (std::vector<jint>{3, 0, 3, 0}));
}

TEST(android_communication, missing_jni)
{
  mock_audio_manager jni;
  cubeb_android_communication mode;
  EXPECT_FALSE(mode.acquire(nullptr, nullptr));
  EXPECT_FALSE(mode.acquire(&jni.env, nullptr));
  EXPECT_TRUE(jni.modes.empty());
  EXPECT_EQ(jni.global_refs, 0);
}

TEST(android_communication, failed_acquisition_can_be_retried)
{
  mock_audio_manager jni;
  cubeb_android_communication mode;
  jni.fail_lookup = true;
  EXPECT_FALSE(mode.acquire(&jni.env, &jni.context));
  EXPECT_FALSE(jni.exception);
  EXPECT_EQ(jni.frames, 0);
  EXPECT_EQ(jni.global_refs, 0);

  jni.fail_lookup = false;
  jni.fail_set_mode = true;
  EXPECT_FALSE(mode.acquire(&jni.env, &jni.context));
  EXPECT_FALSE(jni.exception);
  EXPECT_EQ(jni.frames, 0);
  EXPECT_EQ(jni.global_refs, 0);

  jni.fail_set_mode = false;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  mode.release(&jni.env);
  EXPECT_EQ(jni.global_refs, 0);
}

TEST(android_communication, release_exception)
{
  mock_audio_manager jni;
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  jni.fail_set_mode = true;
  mode.release(&jni.env);
  EXPECT_FALSE(jni.exception);
  EXPECT_EQ(jni.global_refs, 0);

  jni.fail_set_mode = false;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  mode.release(&jni.env);
  EXPECT_EQ(jni.global_refs, 0);
}

TEST(android_communication, concurrent_streams)
{
  mock_audio_manager jni;
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 100; ++j) {
        ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
        mode.release(&jni.env);
      }
    });
  }
  for (auto & thread : threads) {
    thread.join();
  }
  EXPECT_EQ(jni.modes, (std::vector<jint>{3}));
  mode.release(&jni.env);
  EXPECT_EQ(jni.modes, (std::vector<jint>{3, 0}));
  EXPECT_EQ(jni.global_refs, 0);
}

TEST(android_communication, bluetooth_route_and_mode_have_same_owner)
{
  mock_audio_manager jni;
  jni.devices.emplace_back(7, 42);
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  mode.release(&jni.env);
  EXPECT_EQ(jni.events, (std::vector<std::string>{"mode:3", "device:42"}));
  mode.release(&jni.env);
  EXPECT_EQ(jni.events, (std::vector<std::string>{"mode:3", "device:42",
                                                  "clear", "mode:0"}));
}

TEST(android_communication, legacy_bluetooth)
{
  mock_audio_manager jni;
  jni.sdk = 30;
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  mode.release(&jni.env);
  mode.release(&jni.env);
  EXPECT_EQ(jni.events,
            (std::vector<std::string>{"mode:3", "start_sco", "sco:on",
                                      "stop_sco", "sco:off", "mode:0"}));
}

TEST(android_communication, device_priority)
{
  mock_audio_manager jni;
  jni.devices.emplace_back(7, 1);
  jni.devices.emplace_back(26, 2);
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  EXPECT_EQ(jni.events.back(), "device:2");
  jni.devices.emplace_back(22, 3);
  mode.refresh(&jni.env);
  EXPECT_EQ(jni.events.back(), "device:3");
  mode.release(&jni.env);
}

TEST(android_communication, rejected_route_can_be_retried)
{
  mock_audio_manager jni;
  jni.devices.emplace_back(7, 42);
  jni.reject_device = true;
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  jni.reject_device = false;
  mode.refresh(&jni.env);
  mode.release(&jni.env);
  EXPECT_EQ(jni.events,
            (std::vector<std::string>{"mode:3", "device:42", "device:42",
                                      "clear", "mode:0"}));
}

TEST(android_communication, route_exception_does_not_leak)
{
  mock_audio_manager jni;
  jni.devices.emplace_back(7, 42);
  jni.throw_device = true;
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  EXPECT_FALSE(jni.exception);
  mode.release(&jni.env);
  EXPECT_EQ(jni.global_refs, 0);
  EXPECT_EQ(jni.frames, 0);
  EXPECT_EQ(jni.events.back(), "mode:0");
}

TEST(android_communication, disconnect_and_reconnect)
{
  mock_audio_manager jni;
  jni.devices.emplace_back(7, 42);
  cubeb_android_communication mode;
  ASSERT_TRUE(mode.acquire(&jni.env, &jni.context));
  mode.refresh(&jni.env);
  EXPECT_EQ(jni.events.size(), 2u);
  jni.devices.clear();
  mode.refresh(&jni.env);
  EXPECT_EQ(jni.events.back(), "clear");
  jni.devices.emplace_back(26, 43);
  mode.refresh(&jni.env);
  EXPECT_EQ(jni.events.back(), "device:43");
  mode.release(&jni.env);
  EXPECT_EQ(jni.frames, 0);
}
