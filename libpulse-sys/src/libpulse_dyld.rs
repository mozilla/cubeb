#![allow(non_camel_case_types)]

use libpulse::*;
use libc::{c_char,c_double,c_float,c_int,c_uint,c_void};
use libc::size_t;
use libc::{int64_t,uint32_t};

const LIB_NAME: &'static str = "libpulse.so.0";

shared_library!{ libpulse, LIB_NAME,
    pub fn pa_get_library_version() -> *const i8,
    pub fn pa_channel_map_can_balance(map: *const pa_channel_map) -> c_int,
    pub fn pa_channel_map_init(m: *mut pa_channel_map) -> *mut pa_channel_map,
    pub fn pa_context_connect(c: *mut pa_context,
                              server: *const c_char,
                              flags: pa_context_flags_t,
                              api: *const pa_spawn_api) -> c_int,
    pub fn pa_context_disconnect(c: *mut pa_context) -> (),
    pub fn pa_context_drain(c: *mut pa_context, cb: pa_context_notify_cb_t,
                            userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_get_server_info(c: *const pa_context,
                                      cb: pa_server_info_cb_t,
                                      userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_get_sink_info_by_name(c: *const pa_context,
                                            name: *const c_char,
                                            cb: pa_sink_info_cb_t,
                                            userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_get_sink_info_list(c: *const pa_context,
                                         cb: pa_sink_info_cb_t,
                                         userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_get_sink_input_info(c: *const pa_context, idx: uint32_t,
                                          cb: pa_sink_input_info_cb_t,
                                          userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_get_source_info_list(c: *const pa_context,
                                           cb: pa_source_info_cb_t,
                                           userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_get_state(c: *const pa_context) -> pa_context_state_t,
    pub fn pa_context_new(mainloop: *mut pa_mainloop_api,
                          name: *const c_char) -> *mut pa_context,
    pub fn pa_context_rttime_new(c: *const pa_context, usec: pa_usec_t,
                                 cb: pa_time_event_cb_t,
                                 userdata: *mut c_void)
     -> *mut pa_time_event,
    pub fn pa_context_set_sink_input_volume(c: *mut pa_context, idx: uint32_t,
                                            volume: *const pa_cvolume,
                                            cb: pa_context_success_cb_t,
                                            userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_set_state_callback(c: *mut pa_context,
                                         cb: pa_context_notify_cb_t,
                                         userdata: *mut c_void) -> (),
    pub fn pa_context_set_subscribe_callback(c: *mut pa_context,
                                             cb: pa_context_subscribe_cb_t,
                                             userdata: *mut c_void)
     -> (),
    pub fn pa_context_subscribe(c: *mut pa_context, m: pa_subscription_mask_t,
                                cb: pa_context_success_cb_t,
                                userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_context_unref(c: *mut pa_context) -> (),
    pub fn pa_cvolume_set(a: *mut pa_cvolume, channels: c_uint,
                          v: pa_volume_t) -> *mut pa_cvolume,
    pub fn pa_cvolume_set_balance(v: *mut pa_cvolume,
                                  map: *const pa_channel_map,
                                  new_balance: c_float)
     -> *mut pa_cvolume,
    pub fn pa_frame_size(spec: *const pa_sample_spec) -> size_t,
    pub fn pa_mainloop_api_once(m: *mut pa_mainloop_api,
                                callback:
                                    ::std::option::Option<unsafe extern "C" fn(m:
                                                                                   *mut pa_mainloop_api,
                                                                               userdata:
                                                                                   *mut c_void)
                                                              -> ()>,
                                userdata: *mut c_void) -> (),
    pub fn pa_operation_get_state(o: *const pa_operation)
     -> pa_operation_state_t,
    pub fn pa_operation_unref(o: *mut pa_operation) -> (),
    pub fn pa_proplist_gets(p: *mut pa_proplist, key: *const c_char)
     -> *const c_char,
    pub fn pa_rtclock_now() -> pa_usec_t,
    pub fn pa_stream_begin_write(p: *mut pa_stream,
                                 data: *mut *mut c_void,
                                 nbytes: *mut size_t) -> c_int,
    pub fn pa_stream_cancel_write(p: *mut pa_stream) -> c_int,
    pub fn pa_stream_connect_playback(s: *mut pa_stream,
                                      dev: *const c_char,
                                      attr: *const pa_buffer_attr,
                                      flags: pa_stream_flags_t,
                                      volume: *const pa_cvolume,
                                      sync_stream: *const pa_stream)
     -> c_int,
    pub fn pa_stream_connect_record(s: *mut pa_stream,
                                    dev: *const c_char,
                                    attr: *const pa_buffer_attr,
                                    flags: pa_stream_flags_t)
     -> c_int,
    pub fn pa_stream_cork(s: *mut pa_stream, b: c_int,
                          cb: pa_stream_success_cb_t,
                          userdata: *mut c_void) -> *mut pa_operation,
    pub fn pa_stream_disconnect(s: *mut pa_stream) -> c_int,
    pub fn pa_stream_drop(p: *mut pa_stream) -> c_int,
    pub fn pa_stream_get_buffer_attr(s: *const pa_stream)
     -> *const pa_buffer_attr,
    pub fn pa_stream_get_channel_map(s: *const pa_stream)
     -> *const pa_channel_map,
    pub fn pa_stream_get_device_name(s: *const pa_stream)
     -> *const c_char,
    pub fn pa_stream_get_index(s: *const pa_stream) -> uint32_t,
    pub fn pa_stream_get_latency(s: *const pa_stream, r_usec: *mut pa_usec_t,
                                 negative: *mut c_int)
     -> c_int,
    pub fn pa_stream_get_sample_spec(s: *const pa_stream)
     -> *const pa_sample_spec,
    pub fn pa_stream_get_state(p: *const pa_stream) -> pa_stream_state_t,
    pub fn pa_stream_get_time(s: *const pa_stream, r_usec: *mut pa_usec_t)
     -> c_int,
    pub fn pa_stream_new(c: *mut pa_context, name: *const c_char,
                         ss: *const pa_sample_spec,
                         map: *const pa_channel_map) -> *mut pa_stream,
    pub fn pa_stream_peek(p: *mut pa_stream, data: *mut *const c_void,
                          nbytes: *mut size_t) -> c_int,
    pub fn pa_stream_readable_size(p: *const pa_stream) -> size_t,
    pub fn pa_stream_set_state_callback(s: *mut pa_stream,
                                        cb: pa_stream_notify_cb_t,
                                        userdata: *mut c_void) -> (),
    pub fn pa_stream_set_write_callback(p: *mut pa_stream,
                                                    cb: pa_stream_request_cb_t,
                                                    userdata: *mut c_void) -> (),
    pub fn pa_stream_set_read_callback(p: *mut pa_stream,
                                                   cb: pa_stream_request_cb_t,
                                                   userdata: *mut c_void) -> (),
    pub fn pa_stream_unref(s: *mut pa_stream) -> (),
    pub fn pa_stream_update_timing_info(p: *mut pa_stream,
                                        cb: pa_stream_success_cb_t,
                                        userdata: *mut c_void)
     -> *mut pa_operation,
    pub fn pa_stream_writable_size(p: *const pa_stream) -> size_t,
    pub fn pa_stream_write(p: *mut pa_stream, data: *const c_void,
                           nbytes: size_t, free_cb: pa_free_cb_t,
                           offset: int64_t, seek: pa_seek_mode_t)
     -> c_int,
    pub fn pa_sw_volume_from_linear(v: c_double) -> pa_volume_t,
    pub fn pa_threaded_mainloop_free(m: *mut pa_threaded_mainloop) -> (),
    pub fn pa_threaded_mainloop_get_api(m: *mut pa_threaded_mainloop)
     -> *mut pa_mainloop_api,
    pub fn pa_threaded_mainloop_in_thread(m: *mut pa_threaded_mainloop)
     -> c_int,
    pub fn pa_threaded_mainloop_lock(m: *mut pa_threaded_mainloop) -> (),
    pub fn pa_threaded_mainloop_new() -> *mut pa_threaded_mainloop,
    pub fn pa_threaded_mainloop_signal(m: *mut pa_threaded_mainloop,
                                       wait_for_accept: c_int) -> (),
    pub fn pa_threaded_mainloop_start(m: *mut pa_threaded_mainloop)
     -> c_int,
    pub fn pa_threaded_mainloop_stop(m: *mut pa_threaded_mainloop) -> (),
    pub fn pa_threaded_mainloop_unlock(m: *mut pa_threaded_mainloop) -> (),
    pub fn pa_threaded_mainloop_wait(m: *mut pa_threaded_mainloop) -> (),
    pub fn pa_usec_to_bytes(t: pa_usec_t, spec: *const pa_sample_spec)
     -> size_t,
    pub fn pa_xfree(ptr: *mut c_void) -> (),
    pub fn pa_xstrdup(str: *const c_char) -> *mut c_char,
    pub fn pa_xrealloc(ptr: *mut c_void, size: size_t) -> *mut c_void,
}
