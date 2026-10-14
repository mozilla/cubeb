/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef CUBEB_ANDROID_COMMUNICATION_H
#define CUBEB_ANDROID_COMMUNICATION_H

#include "cubeb_log.h"
#include <cassert>
#include <jni.h>
#include <mutex>

class cubeb_android_communication {
public:
  bool acquire(JNIEnv * env, jobject context)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (users) {
      if (env && modern_routing) {
        update_route(env);
      }
      ++users;
      return true;
    }
    if (!env || !context) {
      return false;
    }
    if (env->PushLocalFrame(8) < 0) {
      clear_exception(env);
      return false;
    }
    auto fail = [&] {
      clear_exception(env);
      env->PopLocalFrame(nullptr);
      LOG("Error: could not acquire Android communication mode");
      return false;
    };

    jclass context_class = env->GetObjectClass(context);
    if (!context_class) {
      return fail();
    }
    jmethodID get_service =
        env->GetMethodID(context_class, "getSystemService",
                         "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!get_service) {
      return fail();
    }
    jstring service_name = env->NewStringUTF("audio");
    if (!service_name) {
      return fail();
    }
    jobject local_manager =
        env->CallObjectMethod(context, get_service, service_name);
    if (clear_exception(env) || !local_manager) {
      return fail();
    }
    jclass manager_class = env->GetObjectClass(local_manager);
    if (!manager_class) {
      return fail();
    }
    jmethodID local_set_mode =
        env->GetMethodID(manager_class, "setMode", "(I)V");
    if (!local_set_mode) {
      return fail();
    }
    jobject global_manager = env->NewGlobalRef(local_manager);
    if (!global_manager) {
      return fail();
    }
    env->CallVoidMethod(local_manager, local_set_mode,
                        3 /* MODE_IN_COMMUNICATION */);
    if (clear_exception(env)) {
      env->DeleteGlobalRef(global_manager);
      return fail();
    }
    manager = global_manager;
    set_mode = local_set_mode;
    if (!configure_routing(env, manager_class)) {
      finish(env);
      return fail();
    }
    users = 1;
    update_route(env);
    env->PopLocalFrame(nullptr);
    return true;
  }

  void release(JNIEnv * env)
  {
    std::lock_guard<std::mutex> lock(mutex);
    assert(users > 0);
    if (--users) {
      return;
    }
    assert(env);
    finish(env);
  }

  void refresh(JNIEnv * env)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (users && modern_routing) {
      update_route(env);
    }
  }

private:
  bool configure_routing(JNIEnv * env, jclass manager_class)
  {
    jclass version = env->FindClass("android/os/Build$VERSION");
    if (!version) {
      return false;
    }
    jfieldID sdk_int = env->GetStaticFieldID(version, "SDK_INT", "I");
    if (!sdk_int) {
      return false;
    }
    modern_routing = env->GetStaticIntField(version, sdk_int) >= 31;
    if (clear_exception(env)) {
      return false;
    }
    if (modern_routing) {
      available_devices =
          env->GetMethodID(manager_class, "getAvailableCommunicationDevices",
                           "()Ljava/util/List;");
      if (!available_devices) {
        return false;
      }
      set_device = env->GetMethodID(manager_class, "setCommunicationDevice",
                                    "(Landroid/media/AudioDeviceInfo;)Z");
      if (!set_device) {
        return false;
      }
      clear_device =
          env->GetMethodID(manager_class, "clearCommunicationDevice", "()V");
      return clear_device != nullptr;
    }
    start_sco = env->GetMethodID(manager_class, "startBluetoothSco", "()V");
    if (!start_sco) {
      return false;
    }
    stop_sco = env->GetMethodID(manager_class, "stopBluetoothSco", "()V");
    if (!stop_sco) {
      return false;
    }
    set_sco = env->GetMethodID(manager_class, "setBluetoothScoOn", "(Z)V");
    return set_sco != nullptr;
  }

  static int device_priority(jint type)
  {
    switch (type) {
    case 3:  // TYPE_WIRED_HEADSET
    case 4:  // TYPE_WIRED_HEADPHONES
    case 11: // TYPE_USB_DEVICE
    case 22: // TYPE_USB_HEADSET
      return 3;
    case 26: // TYPE_BLE_HEADSET
    case 27: // TYPE_BLE_SPEAKER
      return 2;
    case 7: // TYPE_BLUETOOTH_SCO
      return 1;
    default:
      return 0;
    }
  }

  void update_route(JNIEnv * env)
  {
    if (!modern_routing) {
      route_requested = true;
      env->CallVoidMethod(manager, start_sco);
      if (clear_exception(env)) {
        LOG("Error: could not start Bluetooth SCO");
        return;
      }
      env->CallVoidMethod(manager, set_sco, JNI_TRUE);
      clear_exception(env);
      return;
    }
    if (env->PushLocalFrame(16) < 0) {
      clear_exception(env);
      return;
    }
    auto done = [&] {
      if (clear_exception(env)) {
        LOG("Error: could not select Android communication device");
      }
      env->PopLocalFrame(nullptr);
    };
    jobject devices = env->CallObjectMethod(manager, available_devices);
    if (env->ExceptionCheck() || !devices) {
      done();
      return;
    }
    jclass list_class = env->GetObjectClass(devices);
    if (!list_class) {
      done();
      return;
    }
    jmethodID size_method = env->GetMethodID(list_class, "size", "()I");
    if (!size_method) {
      done();
      return;
    }
    jmethodID get_method =
        env->GetMethodID(list_class, "get", "(I)Ljava/lang/Object;");
    if (!get_method) {
      done();
      return;
    }
    jint size = env->CallIntMethod(devices, size_method);
    if (env->ExceptionCheck()) {
      done();
      return;
    }
    jobject selected = nullptr;
    int priority = 0;
    for (jint i = 0; i < size; ++i) {
      jobject device = env->CallObjectMethod(devices, get_method, i);
      if (env->ExceptionCheck() || !device) {
        done();
        return;
      }
      jclass device_class = env->GetObjectClass(device);
      if (!device_class) {
        done();
        return;
      }
      jmethodID get_type = env->GetMethodID(device_class, "getType", "()I");
      env->DeleteLocalRef(device_class);
      if (!get_type) {
        done();
        return;
      }
      jint type = env->CallIntMethod(device, get_type);
      if (env->ExceptionCheck()) {
        done();
        return;
      }
      int candidate_priority = device_priority(type);
      if (candidate_priority > priority) {
        env->DeleteLocalRef(selected);
        selected = device;
        priority = candidate_priority;
      } else {
        env->DeleteLocalRef(device);
      }
    }
    if (selected) {
      jclass device_class = env->GetObjectClass(selected);
      if (!device_class) {
        done();
        return;
      }
      jmethodID get_id = env->GetMethodID(device_class, "getId", "()I");
      if (!get_id) {
        done();
        return;
      }
      jint id = env->CallIntMethod(selected, get_id);
      if (env->ExceptionCheck()) {
        done();
        return;
      }
      if (route_requested && id == selected_device_id) {
        done();
        return;
      }
      jboolean accepted = env->CallBooleanMethod(manager, set_device, selected);
      if (env->ExceptionCheck()) {
        done();
        return;
      }
      if (accepted) {
        route_requested = true;
        selected_device_id = id;
      } else {
        LOG("Android communication device request rejected");
      }
    } else if (route_requested) {
      env->CallVoidMethod(manager, clear_device);
      route_requested = false;
    }
    done();
  }

  void finish(JNIEnv * env)
  {
    clear_exception(env);
    if (route_requested) {
      if (modern_routing) {
        env->CallVoidMethod(manager, clear_device);
        clear_exception(env);
      } else {
        env->CallVoidMethod(manager, stop_sco);
        clear_exception(env);
        env->CallVoidMethod(manager, set_sco, JNI_FALSE);
        clear_exception(env);
      }
    }
    // MODE_NORMAL releases our request; getMode() can describe another app's
    // mode.
    env->CallVoidMethod(manager, set_mode, 0 /* MODE_NORMAL */);
    if (clear_exception(env)) {
      LOG("Error: could not release Android communication mode");
    }
    env->DeleteGlobalRef(manager);
    manager = nullptr;
    route_requested = false;
    selected_device_id = 0;
  }

  static bool clear_exception(JNIEnv * env)
  {
    if (!env->ExceptionCheck()) {
      return false;
    }
    env->ExceptionClear();
    return true;
  }

  std::mutex mutex;
  unsigned int users = 0;
  jobject manager = nullptr;
  jmethodID set_mode = nullptr;
  jmethodID available_devices = nullptr;
  jmethodID set_device = nullptr;
  jmethodID clear_device = nullptr;
  jmethodID start_sco = nullptr;
  jmethodID stop_sco = nullptr;
  jmethodID set_sco = nullptr;
  bool modern_routing = false;
  bool route_requested = false;
  jint selected_device_id = 0;
};

#endif
