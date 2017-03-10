use libc::c_void;
use backend;
use cubeb;

extern "C" fn capi_init(c: *mut *mut cubeb::Context, context_name: *const i8) -> i32
{
    unsafe {
        match backend::Context::new(context_name) {
            Ok(ctx) => { *c = Box::into_raw(ctx) as *mut _; cubeb::OK }
            Err(e) => e
        }
    }
}

extern "C" fn capi_get_backend_id(_: *mut cubeb::Context) -> *const i8
{
    "pulse-rust\0".as_ptr() as *const i8
}

extern "C" fn capi_get_max_channel_count(c: *mut cubeb::Context, max_channels: *mut u32) -> i32
{
    let ctx = unsafe { &*(c as *mut backend::Context) };

    match ctx.max_channel_count() {
        Ok(mc) => { unsafe { *max_channels = mc }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_get_min_latency(c: *mut cubeb::Context,
                                   param: cubeb::StreamParams,
                                   latency_frames: *mut u32) -> i32
{
    let ctx = unsafe { &*(c as *mut backend::Context) };

    match ctx.min_latency(&param) {
        Ok(l) => { unsafe { *latency_frames = l }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_get_preferred_sample_rate(c: *mut cubeb::Context, rate: *mut u32) -> i32
{
    let ctx = unsafe { &*(c as *mut backend::Context) };

    match ctx.preferred_sample_rate() {
        Ok(r) => { unsafe { *rate = r }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_get_preferred_channel_layout(c: *mut cubeb::Context, layout: *mut cubeb::ChannelLayout) -> i32
{
    let ctx = unsafe { &*(c as *mut backend::Context) };

    match ctx.preferred_channel_layout() {
        Ok(l) => { unsafe { *layout = l }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_enumerate_devices(c: *mut cubeb::Context,
                                     devtype: cubeb::DeviceType,
                                     collection: *mut *mut cubeb::DeviceCollection) -> i32
{
    let ctx = unsafe { &*(c as *mut backend::Context) };

    match ctx.enumerate_devices(devtype) {
        Ok(dc) => { unsafe { *collection = dc }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_destroy(c: *mut cubeb::Context) {
    let _: Box<backend::Context> = unsafe { Box::from_raw(c as *mut _) };
}

extern "C" fn capi_stream_init(c: *mut cubeb::Context,
                               s: *mut *mut cubeb::Stream,
                               stream_name: *const i8,
                               input_device: cubeb::DeviceId,
                               input_stream_params: *mut cubeb::StreamParams,
                               output_device: cubeb::DeviceId,
                               output_stream_params: *mut cubeb::StreamParams,
                               latency_frames: u32,
                               data_callback: cubeb::DataCallback,
                               state_callback: cubeb::StateCallback,
                               user_ptr: *mut c_void) -> i32
{
    let mut ctx = unsafe { &mut *(c as *mut backend::Context) };

    match ctx.new_stream(stream_name,
                         input_device,
                         if input_stream_params.is_null() { None } else { unsafe { Some(*input_stream_params) } },
                         output_device,
                         if output_stream_params.is_null() { None } else { unsafe { Some(*output_stream_params) } },
                         latency_frames,
                         data_callback,
                         state_callback,
                         user_ptr) {
        Ok(stm) => { unsafe { *s = Box::into_raw(stm) as *mut _ }; cubeb::OK }
        Err(e) => e
    }
}

extern "C" fn capi_stream_destroy(s: *mut cubeb::Stream)
{
    let _ = unsafe { Box::from_raw(s as *mut backend::Stream) };
}

extern "C" fn capi_stream_start(s: *mut cubeb::Stream) -> i32
{
    let mut stm = unsafe { &mut *(s as *mut backend::Stream) };

    stm.start()
}

extern "C" fn capi_stream_stop(s: *mut cubeb::Stream) -> i32
{
    let mut stm = unsafe { &mut *(s as *mut backend::Stream) };

    stm.stop()
}

extern "C" fn capi_stream_get_position(s: *mut cubeb::Stream,
                                       position: *mut u64) -> i32
{
    let stm = unsafe { &*(s as *mut backend::Stream) };

    match stm.position() {
        Ok(pos) => { unsafe { *position = pos }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_stream_get_latency(s: *mut cubeb::Stream,
                                      latency: *mut u32) -> i32
{
    let stm = unsafe { &*(s as *mut backend::Stream) };

    match stm.latency() {
        Ok(lat) => { unsafe { *latency = lat }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_stream_set_volume(s: *mut cubeb::Stream, volume: f32) -> i32
{
    let stm = unsafe { &mut *(s as *mut backend::Stream) };

    stm.set_volume(volume)
}

extern "C" fn capi_stream_set_panning(s: *mut cubeb::Stream, panning: f32) -> i32
{
    let stm = unsafe { &mut *(s as *mut backend::Stream) };

    stm.set_panning(panning)
}

extern "C" fn capi_stream_get_current_device(s: *mut cubeb::Stream, device: *mut *const cubeb::Device) -> i32
{
    let stm = unsafe { &*(s as *mut backend::Stream) };

    match stm.current_device() {
        Ok(d) => { unsafe { *device = Box::into_raw(d) as *mut _ }; cubeb::OK },
        Err(e) => e
    }
}

extern "C" fn capi_stream_device_destroy(_: *mut cubeb::Stream, device: *mut cubeb::Device) -> i32
{
    let _: Box<backend::Device> = unsafe { Box::from_raw(device as *mut backend::Device) };
    cubeb::OK
}

extern "C" fn capi_register_device_collection_changed(c: *mut cubeb::Context,
                                                      devtype: cubeb::DeviceType,
                                                      collection_changed_callback: cubeb::DeviceCollectionChangedCallback,
                                                      user_ptr: *mut c_void) -> i32
{
    let mut ctx = unsafe { &mut *(c as *mut backend::Context) };
    ctx.register_device_collection_changed(devtype, collection_changed_callback, user_ptr)
}

pub const PULSE_OPS: cubeb::Ops = cubeb::Ops {
  init: Some(capi_init),
  get_backend_id: Some(capi_get_backend_id),
  get_max_channel_count: Some(capi_get_max_channel_count),
  get_min_latency: Some(capi_get_min_latency),
  get_preferred_sample_rate: Some(capi_get_preferred_sample_rate),
  get_preferred_channel_layout: Some(capi_get_preferred_channel_layout),
  enumerate_devices: Some(capi_enumerate_devices),
  destroy: Some(capi_destroy),
  stream_init: Some(capi_stream_init),
  stream_destroy: Some(capi_stream_destroy),
  stream_start: Some(capi_stream_start),
  stream_stop: Some(capi_stream_stop),
  stream_get_position: Some(capi_stream_get_position),
  stream_get_latency: Some(capi_stream_get_latency),
  stream_set_volume: Some(capi_stream_set_volume),
  stream_set_panning: Some(capi_stream_set_panning),
  stream_get_current_device: Some(capi_stream_get_current_device),
  stream_device_destroy: Some(capi_stream_device_destroy),
  stream_register_device_changed_callback: None,
  register_device_collection_changed: Some(capi_register_device_collection_changed)
};

#[no_mangle]
pub extern "C" fn pulse_rust_init(c: *mut *mut cubeb::Context, context_name: *const i8) -> i32 {
    capi_init(c, context_name)
}
