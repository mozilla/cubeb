// Copyright © 2017 Mozilla Foundation
//
// This program is made available under an ISC-style license.  See the
// accompanying file LICENSE for details.

use ffi;
use std::ffi::CStr;
use std::os::raw::c_char;

#[derive(Debug)]
pub struct Proplist(*mut ffi::pa_proplist);

impl Proplist {
    pub fn gets(&self, key: *const c_char) -> Option<&CStr> {
        let r = unsafe { ffi::pa_proplist_gets(self.0, key) };
        if r.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(r) })
        }
    }
}

pub unsafe fn from_raw_ptr(raw: *mut ffi::pa_proplist) -> Proplist {
    return Proplist(raw);
}
