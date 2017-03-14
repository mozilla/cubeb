//! Heavily modified version of libpulse-sys to use dynamic symbol
//! loading.
#![recursion_limit="256"]

extern crate libc;

#[macro_use]
extern crate shared_library;

#[allow(non_camel_case_types)]
mod libpulse;

mod libpulse_dyld;
mod libpulse_fns;

pub use libpulse::*;
pub use libpulse_fns::*;
