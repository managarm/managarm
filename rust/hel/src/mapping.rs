use std::ffi::c_void;
use std::ptr::NonNull;

use crate::{Handle, Result, hel_check};

bitflags::bitflags! {
    /// Flags that control the characteristics of a memory mapping.
    /// These flags are used when creating a new memory mapping.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct MappingFlags: u32 {
        /// The mapping pages may be read.
        const READ = hel_sys::kHelMapProtRead;
        /// The mapping pages may be written to.
        const WRITE = hel_sys::kHelMapProtWrite;
        /// The mapping pages may be executed.
        const EXECUTE = hel_sys::kHelMapProtExecute;
        /// If set, accessing an unmapped page in a managed memory mapping
        /// will cause a segfault instead of requesting the page from
        /// the user-space backend. Useful for detecting deadlocks and to
        /// prevent self-blocking page faults.
        const DONT_REQUIRE_BACKING = hel_sys::kHelMapDontRequireBacking;
        /// The address of the mapping is fixed. When mappings overlap,
        /// the old mapping will be replaced by the new one.
        const FIXED = hel_sys::kHelMapFixed;
        /// The address of the mapping is fixed. If another mapping
        /// already exists at the same address, it will not be replaced
        /// and the mapping will fail.
        const FIXED_NO_REPLACE = hel_sys::kHelMapFixedNoReplace;
    }
}

/// A Hel memory mapping.
pub struct Mapping<T> {
    space_handle: hel_sys::HelHandle,
    mapping: Option<NonNull<T>>,
    size: usize,
}

impl<T> Mapping<T> {
    /// Creates a new memory mapping in the given memory space.
    pub fn new_with_space(
        handle: &Handle,
        space: &Handle,
        pointer: usize,
        offset: usize,
        size: usize,
        flags: MappingFlags,
    ) -> Result<Self> {
        let mut mapping = core::ptr::null_mut();

        hel_check(unsafe {
            hel_sys::helMapMemory(
                handle.handle(),
                space.handle(),
                pointer as *mut c_void,
                offset,
                size,
                flags.bits(),
                &mut mapping,
            )
        })?;

        assert!(!mapping.is_null());

        Ok(Self {
            space_handle: space.handle(),
            mapping: Some(NonNull::new(mapping.cast()).unwrap()),
            size,
        })
    }

    /// Creates a new memory mapping in the current thread's memory space.
    pub fn new(
        handle: &Handle,
        pointer: usize,
        offset: usize,
        size: usize,
        flags: MappingFlags,
    ) -> Result<Self> {
        Self::new_with_space(handle, Handle::null(), pointer, offset, size, flags)
    }

    /// Returns a reference to the mapped memory.
    /// This reference is valid until the mapping is unmaped.
    pub fn as_ref(&self) -> Option<&T> {
        self.mapping.map(|mapping| unsafe { mapping.as_ref() })
    }

    /// Returns a mutable reference to the mapped memory.
    /// This reference is valid until the mapping is unmaped.
    pub fn as_mut(&mut self) -> Option<&mut T> {
        self.mapping.map(|mut mapping| unsafe { mapping.as_mut() })
    }

    /// Returns a pointer to the mapped memory.
    /// This pointer is valid until the mapping is unmaped.
    pub fn as_ptr(&self) -> Option<NonNull<T>> {
        self.mapping
    }

    /// Returns the size of the mapping.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Unmaps the memory mapping.
    /// This will invalidate the pointer returned by `as_ptr()`.
    pub fn unmap(&mut self) {
        if let Some(mapping) = self.mapping.take() {
            hel_check(unsafe {
                hel_sys::helUnmapMemory(self.space_handle, mapping.as_ptr().cast(), self.size)
            })
            .expect("Failed to unmap memory");
        }
    }
}

impl<T> Drop for Mapping<T> {
    fn drop(&mut self) {
        self.unmap();
    }
}
