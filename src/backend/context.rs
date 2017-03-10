// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use backend::*;
use backend::cork_state::CorkState;
use backend::var_array::VarArray;
use capi::PULSE_OPS;
use cubeb;
use libc::strcmp;
use libc::{c_char,c_void};
use libpulse_sys::*;
use std::default::Default;
use std::ptr;

macro_rules! dup_str {
    ($Dst: expr, $Src: expr) => {
        if !$Dst.is_null() {
            pa_xfree($Dst as *mut _);
        }

        $Dst = pa_xstrdup($Src);
    }
}

const PA_RATE_MAX: u32 = 48000*8;

fn pa_channel_to_cubeb_channel(channel: pa_channel_position_t) -> cubeb::Channel
{
    assert!(channel != PA_CHANNEL_POSITION_INVALID);
    match channel {
        PA_CHANNEL_POSITION_MONO => cubeb::Channel::Mono,
        PA_CHANNEL_POSITION_FRONT_LEFT => cubeb::Channel::Left,
        PA_CHANNEL_POSITION_FRONT_RIGHT => cubeb::Channel::Right,
        PA_CHANNEL_POSITION_FRONT_CENTER => cubeb::Channel::Center,
        PA_CHANNEL_POSITION_SIDE_LEFT => cubeb::Channel::LeftSurround,
        PA_CHANNEL_POSITION_SIDE_RIGHT => cubeb::Channel::RightSurround,
        PA_CHANNEL_POSITION_REAR_LEFT => cubeb::Channel::RearLeftSurround,
        PA_CHANNEL_POSITION_REAR_CENTER => cubeb::Channel::RearCenter,
        PA_CHANNEL_POSITION_REAR_RIGHT => cubeb::Channel::RearRightSurround,
        PA_CHANNEL_POSITION_LFE => cubeb::Channel::LowFrequency,
        _ => cubeb::Channel::Invalid
    }
}

fn channel_map_to_layout(cm: &pa_channel_map) -> cubeb::ChannelLayout
{
    let mut cubeb_map: cubeb::ChannelMap = Default::default();
    cubeb_map.channels = cm.channels as u32;
    for i in 0usize..cm.channels as usize {
        cubeb_map.map[i] = pa_channel_to_cubeb_channel(cm.map[i]);
    }
    unsafe { cubeb::cubeb_channel_map_to_layout(&cubeb_map) }
}

#[derive(Debug)]
pub struct Context
{
    pub ops: *const cubeb::Ops,
    pub mainloop: *mut pa_threaded_mainloop,
    pub context: *mut pa_context,
    pub default_sink_info: *mut pa_sink_info,
    pub context_name: *const i8,
    pub collection_changed_callback: cubeb::DeviceCollectionChangedCallback,
    pub collection_changed_user_ptr: *mut c_void,
    pub error: bool,
}

destroy!(Context);

impl Context
{
    pub fn new(name: *const i8) -> Result<Box<Self>>
    {
        let mut ctx = Box::new(Context{
            ops: &PULSE_OPS,
            mainloop: pa_threaded_mainloop_new(),
            context: 0 as *mut _,
            default_sink_info: 0 as *mut _,
            context_name: name,
            collection_changed_callback: None,
            collection_changed_user_ptr: 0 as *mut _,
            error: true,
        });

        pa_threaded_mainloop_start(ctx.mainloop);

        if ctx.pulse_context_init() != cubeb::OK {
            ctx.destroy();
            return Err(cubeb::ERROR);
        }

        pa_threaded_mainloop_lock(ctx.mainloop);
        pa_context_get_server_info(ctx.context,
                                   Some(server_info_callback),
                                   ctx.as_mut() as *mut Context as *mut _);
        pa_threaded_mainloop_unlock(ctx.mainloop);

        // Return the result.
        Ok(ctx)
    }

    pub fn destroy(&mut self)
    {
        if !self.default_sink_info.is_null() {
            let _ = unsafe { Box::from_raw(self.default_sink_info) };
        }

        if !self.context.is_null() {
            self.pulse_context_destroy();
        }

        if !self.mainloop.is_null() {
            pa_threaded_mainloop_stop(self.mainloop);
            pa_threaded_mainloop_free(self.mainloop);
        }
    }

    pub fn new_stream(&mut self,
                      stream_name: *const i8,
                      input_device: cubeb::DeviceId,
                      input_stream_params: Option<cubeb::StreamParams>,
                      output_device: cubeb::DeviceId,
                      output_stream_params: Option<cubeb::StreamParams>,
                      latency_frames: u32,
                      data_callback: cubeb::DataCallback,
                      state_callback: cubeb::StateCallback,
                      user_ptr: *mut c_void) -> Result<Box<Stream>>
    {
        if self.error && self.pulse_context_init() != 0 {
            return Err(cubeb::ERROR);
        }

        Stream::new(self,
                    stream_name,
                    input_device,
                    input_stream_params,
                    output_device,
                    output_stream_params,
                    latency_frames,
                    data_callback,
                    state_callback,
                    user_ptr)
    }

    pub fn max_channel_count(&self) -> Result<u32>
    {
        unsafe {
            pa_threaded_mainloop_lock(self.mainloop);
            while self.default_sink_info.is_null() {
                pa_threaded_mainloop_wait(self.mainloop);
            }
            pa_threaded_mainloop_unlock(self.mainloop);

            Ok((*self.default_sink_info).channel_map.channels as u32)
        }
    }

    pub fn preferred_sample_rate(&self) -> Result<u32>
    {
        pa_threaded_mainloop_lock(self.mainloop);
        while self.default_sink_info.is_null() {
            pa_threaded_mainloop_wait(self.mainloop);
        }
        pa_threaded_mainloop_unlock(self.mainloop);

        unsafe {
            Ok((*self.default_sink_info).sample_spec.rate)
        }
    }

    pub fn min_latency(&self, params: &cubeb::StreamParams) -> Result<u32>
    {
        // According to PulseAudio developers, this is a safe minimum.
        Ok(25 * params.rate / 1000)
    }

    pub fn preferred_channel_layout(&self) -> Result<cubeb::ChannelLayout>
    {
        pa_threaded_mainloop_lock(self.mainloop);
        while self.default_sink_info.is_null() {
            pa_threaded_mainloop_wait(self.mainloop);
        }
        pa_threaded_mainloop_unlock(self.mainloop);

        unsafe {
            Ok(channel_map_to_layout(&(*self.default_sink_info).channel_map))
        }
    }

    pub fn enumerate_devices(&self, devtype: cubeb::DeviceType) -> Result<*mut cubeb::DeviceCollection>
    {
        let mut user_data: PulseDevListData = Default::default();
        user_data.context = self as *const _ as *mut _;

        pa_threaded_mainloop_lock(self.mainloop);

        let o = pa_context_get_server_info(self.context,
                                           Some(pulse_server_info_cb),
                                           &mut user_data as *mut _ as *mut _);
        if !o.is_null() {
            self.operation_wait(ptr::null_mut(), o);
            pa_operation_unref(o);
        }

        if devtype.is_output() {
            let o = pa_context_get_sink_info_list(self.context,
                                                  Some(pulse_sink_info_cb),
                                                  &mut user_data as *mut _ as *mut _);
            if !o.is_null() {
                self.operation_wait(ptr::null_mut(), o);
                pa_operation_unref(o);
            }
        }

        if devtype.is_input() {
            let o = pa_context_get_source_info_list(self.context,
                                                    Some(pulse_source_info_cb),
                                                    &mut user_data as *mut _ as *mut _);
            if !o.is_null() {
                self.operation_wait(ptr::null_mut(), o);
                pa_operation_unref(o);
            }
        }

        pa_threaded_mainloop_unlock(self.mainloop);

        // TODO: This is dodgy - Need to account for padding between count
        // and device array in C code on 64-bit platforms. Using an extra
        // pointer instead of the header size to achieve this.
        let mut coll: Box<VarArray<*const cubeb::DeviceInfo>> = VarArray::with_length(user_data.devinfo.len());
        for (e1, e2) in user_data.devinfo.drain(..).zip(coll.as_mut_slice().iter_mut()) {
            *e2 = e1;
        }

        Ok(Box::into_raw(coll) as *mut cubeb::DeviceCollection)
    }

    pub fn register_device_collection_changed(&mut self,
                                              devtype: cubeb::DeviceType,
                                              cb: cubeb::DeviceCollectionChangedCallback,
                                              user_ptr: *mut c_void) -> i32
    {
        self.collection_changed_callback = cb;
        self.collection_changed_user_ptr = user_ptr;

        unsafe {
            pa_threaded_mainloop_lock(self.mainloop);

            let mut mask: pa_subscription_mask_t = PA_SUBSCRIPTION_MASK_NULL;
            if self.collection_changed_callback.is_none() {
                // Unregister subscription
                pa_context_set_subscribe_callback(self.context,
                                                  None,
                                                  0 as *mut _);
            } else {
                pa_context_set_subscribe_callback(self.context,
                                                  Some(pulse_subscribe_callback),
                                                  self as *mut _ as *mut _);
                if devtype.is_input() { mask |= PA_SUBSCRIPTION_MASK_SOURCE };
                if devtype.is_output() { mask |= PA_SUBSCRIPTION_MASK_SOURCE };
            }

            let o = pa_context_subscribe(self.context,
                                         mask,
                                         Some(subscribe_success),
                                         self as *const _ as *mut _);
            if o.is_null() {
                log!("Context subscribe failed");
                return cubeb::ERROR;
            }
            self.operation_wait(ptr::null_mut(), o);
            pa_operation_unref(o);

            pa_threaded_mainloop_unlock(self.mainloop);
        }

        cubeb::OK
    }

    //

    pub fn pulse_stream_cork(&self, stream: *mut pa_stream, state: CorkState)
    {
        unsafe extern fn cork_success(_: *mut pa_stream, _: i32, u: *mut c_void)
        {
            let mainloop = u as *mut pa_threaded_mainloop;
            pa_threaded_mainloop_signal(mainloop, 0);
        }

        if stream.is_null() {
            return;
        }

        let o = pa_stream_cork(stream,
                               state.is_cork() as i32,
                               Some(cork_success),
                               self.mainloop as *mut _);

        if !o.is_null() {
            self.operation_wait(stream, o);
            pa_operation_unref(o);
        }
    }

    pub fn pulse_context_init(&mut self) -> i32
    {
        unsafe extern fn error_state(c: *mut pa_context, u: *mut c_void)
        {
            let mut ctx = &mut *(u as *mut Context);
            if !PA_CONTEXT_IS_GOOD(pa_context_get_state(c)) {
                ctx.error = true;
            }
            pa_threaded_mainloop_signal(ctx.mainloop, 0);
        }

        if !self.context.is_null() {
            debug_assert!(self.error);
            self.pulse_context_destroy();
        }

        self.context = pa_context_new(
            pa_threaded_mainloop_get_api(self.mainloop),
            self.context_name);

        if self.context.is_null() {
            return cubeb::ERROR;
        }

        pa_context_set_state_callback(self.context,
                                      Some(error_state),
                                      self as *mut _ as *mut _);

        pa_threaded_mainloop_lock(self.mainloop);
        pa_context_connect(self.context, ptr::null(), 0, ptr::null());

        if !self.wait_until_context_ready() {
            pa_threaded_mainloop_unlock(self.mainloop);
            self.pulse_context_destroy();
            self.context = ptr::null_mut();
            return cubeb::ERROR;
        }

        pa_threaded_mainloop_unlock(self.mainloop);

        self.error = false;

        cubeb::OK
    }

    fn pulse_context_destroy(&mut self)
    {
        unsafe extern fn drain_complete(_c: *mut pa_context, u: *mut c_void)
        {
            let mainloop = u as *mut pa_threaded_mainloop;
            pa_threaded_mainloop_signal(mainloop, 0);
        }

        pa_threaded_mainloop_lock(self.mainloop);
        let o = pa_context_drain(self.context, Some(drain_complete), self.mainloop as *mut _);
        if !o.is_null() {
            self.operation_wait(ptr::null_mut(), o);
            pa_operation_unref(o);
        }
        pa_context_set_state_callback(self.context, None, ptr::null_mut());
        pa_context_disconnect(self.context);
        pa_context_unref(self.context);
        pa_threaded_mainloop_unlock(self.mainloop);
    }

    pub fn operation_wait(&self, stream: *mut pa_stream, o: *mut pa_operation) -> bool
    {
        while pa_operation_get_state(o) == PA_OPERATION_RUNNING {
            pa_threaded_mainloop_wait(self.mainloop);
            if !PA_CONTEXT_IS_GOOD(pa_context_get_state(self.context)) {
                return false;
            }

            if !stream.is_null() && !PA_STREAM_IS_GOOD(pa_stream_get_state(stream)) {
                return false;
            }
        }

        true
    }

    pub fn wait_until_context_ready(&self) -> bool
    {
        loop {
            let state = pa_context_get_state(self.context);
            if !PA_CONTEXT_IS_GOOD(state) {
                return false;
            }
            if state == PA_CONTEXT_READY {
                break;
            }
            pa_threaded_mainloop_wait(self.mainloop);
        }

        true
    }
}


// Callbacks
unsafe extern fn sink_info_callback(_context: *mut pa_context,
                             info: *const pa_sink_info,
                             eol: i32,
                             u: *mut c_void)
{
    let mut ctx = &mut *(u as *mut Context);
    if eol == 0 {
        if !ctx.default_sink_info.is_null() {
            let _ = Box::from_raw(ctx.default_sink_info);
        }
        ctx.default_sink_info = Box::into_raw(Box::new(*info));
    }
    pa_threaded_mainloop_signal(ctx.mainloop, 0);
}

unsafe extern fn server_info_callback(context: *mut pa_context, info: *const pa_server_info, u: *mut c_void)
{
  pa_context_get_sink_info_by_name(context,
                                   (*info).default_sink_name,
                                   Some(sink_info_callback),
                                   u);
}

struct PulseDevListData {
  default_sink_name: *mut c_char,
  default_source_name: *mut c_char,
  devinfo: Vec<*const cubeb::DeviceInfo>,
  context: *mut Context
}

impl Drop for PulseDevListData {
    fn drop(&mut self) {
        for elem in self.devinfo.iter_mut() {
            let _ = unsafe { Box::from_raw(elem) };
        }
        if !self.default_sink_name.is_null() {
            pa_xfree(self.default_sink_name as *mut _);
        }
        if !self.default_source_name.is_null() {
            pa_xfree(self.default_source_name as *mut _);
        }
    }
}

impl Default for PulseDevListData
{
    fn default() -> Self {
        PulseDevListData {
            default_sink_name: 0 as *mut _,
            default_source_name: 0 as *mut _,
            devinfo: Vec::new(),
            context: 0 as *mut _
        }
    }
}

fn pulse_format_to_cubeb_format(format: pa_sample_format_t) -> cubeb::DeviceFmt
{
  match format {
    PA_SAMPLE_S16LE => cubeb::DeviceFmt::s16_le(),
    PA_SAMPLE_S16BE => cubeb::DeviceFmt::s16_be(),
    PA_SAMPLE_FLOAT32LE => cubeb::DeviceFmt::f32_le(),
    PA_SAMPLE_FLOAT32BE => cubeb::DeviceFmt::f32_be(),
    _ => { panic!("Invalid format"); }
  }
}

fn pulse_get_state_from_sink_port(i: *const pa_port_info) -> cubeb::DeviceState
{
    if !i.is_null() {
        let info = unsafe { *i };
        return if cfg!(feature="pa_version_2") && info.available == PA_PORT_AVAILABLE_NO as i32 {
            cubeb::DeviceState::Unplugged
        } else {
            cubeb::DeviceState::Enabled
        }
    }

    cubeb::DeviceState::Disabled
}

unsafe extern fn pulse_sink_info_cb(_context: *mut pa_context,
                                    i: *const pa_sink_info,
                                    eol: i32,
                                    user_data: *mut c_void)
{
    if eol != 0 || i.is_null() {
        return;
    }

    debug_assert!(!user_data.is_null());

    let info = *i;
    let mut list_data = &mut *(user_data as *mut PulseDevListData);

    let device_id = pa_xstrdup(info.name);

    let group_id = {
        let prop = pa_proplist_gets(info.proplist, b"sysfs.path\0".as_ptr() as *const c_char);
        if !prop.is_null() {
            pa_xstrdup(prop)
        } else {
            0 as *mut c_char
        }
    };

    let vendor_name = {
        let prop = pa_proplist_gets(info.proplist, b"device.vendor.name\0".as_ptr() as *const c_char);
        if !prop.is_null() {
            pa_xstrdup(prop)
        } else {
            0 as *mut c_char
        }
    };

    let preferred =
        if strcmp(info.name, list_data.default_sink_name) == 0 {
            cubeb::DevicePref::all()
        } else {
            cubeb::DevicePref::none()
        };

    let devinfo = cubeb::DeviceInfo {
        device_id: device_id,
        devid: device_id as cubeb::DeviceId,
        friendly_name: pa_xstrdup(info.description),
        group_id: group_id,
        vendor_name: vendor_name,
        devtype: cubeb::DeviceType::output(),
        state: pulse_get_state_from_sink_port(info.active_port),
        preferred: preferred,
        format: cubeb::DeviceFmt::all(),
        default_format: pulse_format_to_cubeb_format(info.sample_spec.format),
        max_channels: info.channel_map.channels as u32,
        min_rate: 1,
        max_rate: PA_RATE_MAX,
        default_rate: info.sample_spec.rate,
        latency_lo: 0,
        latency_hi: 0,
    };
    list_data.devinfo.push(Box::into_raw(Box::new(devinfo)));

    pa_threaded_mainloop_signal((*list_data.context).mainloop, 0);
}

fn pulse_get_state_from_source_port(i: *mut pa_port_info) -> cubeb::DeviceState
{
    if !i.is_null() {
        let info = unsafe { *i };
        return if cfg!(feature="pa_version_2") && info.available == PA_PORT_AVAILABLE_NO as i32 {
            cubeb::DeviceState::Unplugged
        } else {
            cubeb::DeviceState::Enabled
        }
    }

    cubeb::DeviceState::Disabled
}

unsafe extern fn pulse_source_info_cb(_context: *mut pa_context,
                                      i: *const pa_source_info,
                                      eol: i32,
                                      user_data: *mut c_void)
{
    if eol != 0 || i.is_null() {
        return;
    }

    debug_assert!(!user_data.is_null());

    let info = *i;
    let mut list_data = &mut *(user_data as *mut PulseDevListData);

    let device_id = pa_xstrdup(info.name);

    let group_id = {
        let prop = pa_proplist_gets(info.proplist, b"sysfs.path\0".as_ptr() as *mut c_char);
        if !prop.is_null() {
            pa_xstrdup(prop)
        } else {
            0 as *mut c_char
        }
    };

    let vendor_name = {
        let prop = pa_proplist_gets(info.proplist, b"device.vendor.name\0".as_ptr() as *mut c_char);
        if !prop.is_null() {
            pa_xstrdup(prop)
        } else {
            0 as *mut c_char
        }
    };

    let preferred =
        if strcmp(info.name, list_data.default_source_name) == 0 {
            cubeb::DevicePref::all()
        } else {
            cubeb::DevicePref::none()
        };

    let devinfo = cubeb::DeviceInfo {
        device_id: device_id,
        devid: device_id as cubeb::DeviceId,
        friendly_name: pa_xstrdup(info.description),
        group_id: group_id,
        vendor_name: vendor_name,
        devtype: cubeb::DeviceType::input(),
        state: pulse_get_state_from_source_port(info.active_port),
        preferred: preferred,
        format: cubeb::DeviceFmt::all(),
        default_format: pulse_format_to_cubeb_format(info.sample_spec.format),
        max_channels: info.channel_map.channels as u32,
        min_rate: 1,
        max_rate: PA_RATE_MAX,
        default_rate: info.sample_spec.rate,
        latency_lo: 0,
        latency_hi: 0,
    };

    list_data.devinfo.push(Box::into_raw(Box::new(devinfo)));

    pa_threaded_mainloop_signal((*list_data.context).mainloop, 0);
}

unsafe extern fn pulse_server_info_cb(_context: *mut pa_context,
                                      i: *const pa_server_info,
                                      user_data: *mut c_void)
{
    assert!(!i.is_null());
    let info = *i;
    let list_data = &mut *(user_data as *mut PulseDevListData);

    dup_str!(list_data.default_sink_name, info.default_sink_name);
    dup_str!(list_data.default_source_name, info.default_source_name);

    pa_threaded_mainloop_signal((*list_data.context).mainloop, 0);
}

unsafe extern fn pulse_subscribe_callback(_ctx: *mut pa_context,
                                          t: pa_subscription_event_type_t,
                                          index: u32,
                                          user_data: *mut c_void)
{
    let mut ctx = &mut *(user_data as *mut Context);

    match t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK {
        PA_SUBSCRIPTION_EVENT_SOURCE |
        PA_SUBSCRIPTION_EVENT_SINK => {

            if cubeb::g_log_level != cubeb::LogLevel::Disabled {
                if (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE &&
                    (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE {
                   log!("Removing sink index %d", index);
                } else if (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE &&
                           (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW {
                    log!("Adding sink index %d", index);
                }
                if (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK &&
                    (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE {
                    log!("Removing source index %d", index);
                } else if (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK &&
                           (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW {
                    log!("Adding source index %d", index);
                }
            }

            if (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE ||
                (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW {
                    ctx.collection_changed_callback.unwrap()(ctx as *mut _ as *mut _,
                                                             ctx.collection_changed_user_ptr);
                }
        },
        _ => {}
    }
}


unsafe extern fn subscribe_success(_: *mut pa_context,
                                   success: i32,
                                   user_data: *mut c_void)
{
    let ctx = &*(user_data as *mut Context);
    debug_assert!(success != 0);
    pa_threaded_mainloop_signal(ctx.mainloop, 0);
}
