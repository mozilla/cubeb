// Copyright © 2017 Mozilla Foundation
//
// This program is made available under an ISC-style license.  See the
// accompanying file LICENSE for details.

use std::ffi::CStr;
use std::os::raw::c_char;

pub trait UnwrapCStr {
    fn unwrap_cstr(self) -> *const c_char;
}

impl<'a, U> UnwrapCStr for U
    where U: Into<Option<&'a CStr>>
{
    fn unwrap_cstr(self) -> *const c_char {
        self.into().map(|o| o.as_ptr()).unwrap_or(0 as *const _)
    }
}
