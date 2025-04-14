use std::ptr::NonNull;
use std::{ffi::c_void, num::NonZeroUsize};

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
    length: usize,
}

impl<T> Mapping<T> {
    /// Creates a new memory mapping for the object described by the
    /// given handle accessible within the given address space. This
    /// function is unsafe because it lets the user create a memory
    /// mapping that may violate the memory safety guarantees of Rust.
    pub unsafe fn new_with_space(
        handle: &Handle,
        space: &Handle,
        pointer: Option<NonZeroUsize>,
        offset: usize,
        length: usize,
        flags: MappingFlags,
    ) -> Result<Self> {
        let mut mapping = std::ptr::null_mut();

        hel_check(unsafe {
            hel_sys::helMapMemory(
                handle.handle(),
                space.handle(),
                pointer.map_or(std::ptr::null_mut(), |p| p.get() as *mut c_void),
                offset,
                length,
                flags.bits(),
                &mut mapping,
            )
        })?;

        assert!(!mapping.is_null());

        Ok(Self {
            space_handle: space.handle(),
            mapping: Some(NonNull::new(mapping.cast()).unwrap()),
            length,
        })
    }

    /// Creates a new memory mapping for the object described by the
    /// given handle accessible within the current thread's address space.
    /// This function is unsafe because it lets the user create a memory
    /// mapping that may violate the memory safety guarantees of Rust.
    pub unsafe fn new(
        handle: &Handle,
        pointer: Option<NonZeroUsize>,
        offset: usize,
        length: usize,
        flags: MappingFlags,
    ) -> Result<Self> {
        unsafe { Self::new_with_space(handle, Handle::null(), pointer, offset, length, flags) }
    }

    /// Returns a reference to the mapped memory.
    /// This reference is valid until the mapping is dropped.
    pub fn as_ref(&self) -> Option<&T> {
        self.mapping.map(|mapping| unsafe { mapping.as_ref() })
    }

    /// Returns a mutable reference to the mapped memory.
    /// This reference is valid until the mapping is dropped.
    pub fn as_mut(&mut self) -> Option<&mut T> {
        self.mapping.map(|mut mapping| unsafe { mapping.as_mut() })
    }

    /// Returns a raw ointer to the mapped memory.
    /// This function is unsafe because the returned pointer
    /// is not subject to Rust's memory safety guarantees.
    /// The pointer is valid until the mapping is dropped.
    pub unsafe fn as_ptr(&self) -> Option<NonNull<T>> {
        self.mapping
    }

    /// Returns the length of the mapping.
    pub fn len(&self) -> usize {
        self.length
    }

    /// Unmaps the memory mapping.
    /// This will invalidate the pointer returned by `as_ptr()`.
    pub fn unmap(&mut self) {
        if let Some(mapping) = self.mapping.take() {
            hel_check(unsafe {
                hel_sys::helUnmapMemory(self.space_handle, mapping.as_ptr().cast(), self.length)
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
