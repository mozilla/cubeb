// Required for dlopen, et al.
#[cfg(feature = "dynamic-link")]
extern crate libc;

mod ffi_types;
mod ffi_funcs;

pub use ffi_types::*;
pub use ffi_funcs::*;

#[cfg(feature = "dynamic-link")]
pub unsafe fn open() -> Option<LibLoader> {
    return LibLoader::open();
}
