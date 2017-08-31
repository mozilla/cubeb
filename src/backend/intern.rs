// Copyright © 2017 Mozilla Foundation
//
// This program is made available under an ISC-style license.  See the
// accompanying file LICENSE for details.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

#[derive(Debug)]
pub struct Intern {
    vec: Vec<Box<CString>>
}

impl Intern {
    pub fn new() -> Intern {
        Intern {
            vec: Vec::new()
        }
    }

    pub fn add(&mut self, string: &CStr) -> *const c_char {
        fn eq(s1: &CStr, s2: &CStr) -> bool { s1 == s2 }
        for s in &self.vec {
            if eq(s, string) {
                return s.as_ptr();
            }
        }

        self.vec.push(Box::new(string.to_owned()));
        self.vec.last().unwrap().as_ptr()
    }
}
