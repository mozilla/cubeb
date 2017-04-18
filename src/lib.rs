//! Cubeb backend interface to Pulse Audio

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#[macro_use]
extern crate cubeb_ffi as cubeb;
extern crate pulse_ffi;
extern crate semver;

mod capi;
mod backend;

pub use capi::pulse_rust_init;
