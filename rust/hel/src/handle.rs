use std::mem::ManuallyDrop;

use crate::result::{Result, hel_check};

/// A handle to a Hel object.
#[derive(Debug)]
pub struct Handle {
    handle: hel_sys::HelHandle,
    universe: hel_sys::HelHandle,
}

/// A static handle that represents a null handle.
/// This is used to represent a non-existent or invalid handle.
static NULL_HANDLE: ManuallyDrop<Handle> = unsafe {
    ManuallyDrop::new(Handle::from_raw_in_universe(
        hel_sys::kHelNullHandle as hel_sys::HelHandle as i64,
        hel_sys::kHelNullHandle as hel_sys::HelHandle as i64,
    ))
};

/// A static handle that refers the current universe.
static THIS_UNIVERSE: ManuallyDrop<Handle> = unsafe {
    ManuallyDrop::new(Handle::from_raw_in_universe(
        hel_sys::kHelThisUniverse as hel_sys::HelHandle as i64,
        hel_sys::kHelNullHandle as hel_sys::HelHandle as i64,
    ))
};

/// A static handle that refers the current thread.
static THIS_THREAD: ManuallyDrop<Handle> = unsafe {
    ManuallyDrop::new(Handle::from_raw_in_universe(
        hel_sys::kHelThisThread as hel_sys::HelHandle as i64,
        hel_sys::kHelNullHandle as hel_sys::HelHandle as i64,
    ))
};

/// A static handle that refers to a zeroed memory view.
static ZERO_MEMORY: ManuallyDrop<Handle> = unsafe {
    ManuallyDrop::new(Handle::from_raw_in_universe(
        hel_sys::kHelZeroMemory as hel_sys::HelHandle as i64,
        hel_sys::kHelNullHandle as hel_sys::HelHandle as i64,
    ))
};

impl Handle {
    /// Returns a reference to the null handle.
    pub fn null() -> &'static Self {
        &NULL_HANDLE
    }

    /// Returns a reference to the current universe handle.
    pub fn this_universe() -> &'static Self {
        &THIS_UNIVERSE
    }

    /// Returns a reference to the current thread handle.
    pub fn this_thread() -> &'static Self {
        &THIS_THREAD
    }

    /// Returns a reference to the zero memory handle.
    pub fn zero_memory() -> &'static Self {
        &ZERO_MEMORY
    }

    pub const unsafe fn from_raw(handle: hel_sys::HelHandle) -> Self {
        Self {
            handle,
            universe: hel_sys::kHelThisUniverse as hel_sys::HelHandle,
        }
    }

    pub const unsafe fn from_raw_in_universe(
        handle: hel_sys::HelHandle,
        universe: hel_sys::HelHandle,
    ) -> Self {
        Self { handle, universe }
    }

    /// Returns the raw handle.
    pub fn handle(&self) -> hel_sys::HelHandle {
        self.handle
    }

    /// Returns the raw handle of the universe this handle belongs to.
    pub(crate) fn universe(&self) -> hel_sys::HelHandle {
        self.universe
    }

    /// Closes the handle, potentially freeing resources.
    pub fn close(&mut self) {
        if self.handle != hel_sys::kHelNullHandle as hel_sys::HelHandle {
            hel_check(unsafe { hel_sys::helCloseDescriptor(self.universe, self.handle) })
                .expect("Failed to close descriptor");

            self.handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
        }
    }

    /// Clones the handle into the given universe. This is useful for
    /// sharing objects between different universes or creating multiple
    /// references to the same object.
    pub fn clone_into(&self, universe: &Handle) -> Result<Self> {
        let mut new_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;

        hel_check(unsafe {
            hel_sys::helTransferDescriptor(self.handle, universe.handle, &mut new_handle)
        })?;

        Ok(Handle {
            handle: new_handle,
            universe: universe.handle,
        })
    }

    /// Clones the handle into the current universe. This function is a convenience
    /// method that calls `clone_into` with the current universe handle.
    pub fn clone_handle(&self) -> Result<Self> {
        self.clone_into(Handle::this_universe())
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.close();
    }
}
