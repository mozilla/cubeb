/// A C-style variable length array implemented as one allocation with
/// a prepended header.
use libpulse_sys::{pa_xrealloc};

#[repr(C)]
#[derive(Debug)]
pub struct VarArray<T>
{
    len: u32,
    data: [T;0],
}

impl<T> VarArray<T>
{
    pub fn len(&self) -> usize {
        self.len as usize
    }

    unsafe fn _realloc(ptr: Option<Box<Self>>, count: usize) -> Box<Self> {
        use std::mem::{size_of, transmute};

        let size = size_of::<Self>() + count * size_of::<T>();
        let raw_ptr = match ptr {
            Some(box_ptr) => Box::into_raw(box_ptr) as *mut u8,
            None => 0 as *mut u8
        };
        let mem = pa_xrealloc(raw_ptr as *mut _, size);
        let mut result: Box<Self> = transmute(mem);
        result.len = count as u32;
        result
    }

    pub fn with_length(len: usize) -> Box<VarArray<T>>
    {
        unsafe {
            Self::_realloc(None, len)
        }
    }

    pub fn as_mut_slice<'r>(&'r mut self) -> &'r mut [T] {
        use std::slice::from_raw_parts_mut;

        unsafe {
            from_raw_parts_mut(&self.data as *const _ as *mut _, self.len())
        }
    }
}

impl<T> Drop for VarArray<T>
{
    fn drop(&mut self) {
        let ptr = self as *mut Self;
        unsafe { Self::_realloc(Some(Box::from_raw(ptr)), 0); }
    }
}
