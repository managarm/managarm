use std::ffi::CStr;
use std::marker::PhantomData;
use std::ptr::NonNull;

use uacpi_sys::{uacpi_object, uacpi_object_array, uacpi_u64};

use super::namespace::NamespaceNode;
use super::{Result, check};

pub struct Object {
    object: NonNull<uacpi_object>,
}

impl Object {
    pub fn package(&self) -> Result<Package<'_>> {
        let mut array = uacpi_object_array::default();
        unsafe {
            check(
                "uacpi_object_get_package",
                uacpi_sys::uacpi_object_get_package(self.object.as_ptr(), &mut array),
            )?;
        }
        Ok(Package {
            array,
            marker: PhantomData,
        })
    }
}

impl Drop for Object {
    fn drop(&mut self) {
        unsafe { uacpi_sys::uacpi_object_unref(self.object.as_ptr()) };
    }
}

pub struct Package<'a> {
    array: uacpi_object_array,
    marker: PhantomData<&'a Object>,
}

impl Package<'_> {
    pub fn integer(&self, index: usize) -> Option<u64> {
        if self.array.count <= index {
            return None;
        }

        let mut value: uacpi_u64 = 0;
        let status = unsafe {
            uacpi_sys::uacpi_object_get_integer(*self.array.objects.add(index), &mut value)
        };
        match status {
            uacpi_sys::UACPI_STATUS_OK => Some(value),
            _ => None,
        }
    }
}

impl NamespaceNode {
    pub fn eval_package(&self, path: &CStr) -> Result<Object> {
        let mut object: *mut uacpi_object = std::ptr::null_mut();
        unsafe {
            check(
                "uacpi_eval_simple_package",
                uacpi_sys::uacpi_eval_simple_package(self.as_raw(), path.as_ptr(), &mut object),
            )?;
        }

        let object = NonNull::new(object).expect("uACPI returned no object");
        Ok(Object { object })
    }
}
