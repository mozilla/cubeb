macro_rules! destroy {
    ($Name: ident) => {
        impl Drop for $Name {
            fn drop(&mut self) {
                self.destroy()
            }
        }
    }
}

mod context;
mod cork_state;
mod stream;
mod var_array;

pub type Result<T> = ::std::result::Result<T, i32>;

pub use self::context::Context;
pub use self::stream::Stream;
pub use self::stream::Device;
