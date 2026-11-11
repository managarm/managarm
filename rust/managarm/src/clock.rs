//! Access to thor's clock page (see `HelClockPage` in `hel.h`).

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("thor's clock is not based on the TSC")]
    NotTsc,
    #[error(transparent)]
    Hel(#[from] hel::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

/// The frequency (in Hz) of the TSC, as thor calibrated it. Derived from the tick -> nanosecond
/// conversion of helGetClock() on thor's clock page; fails if that clock is not the TSC.
pub fn tsc_hz() -> Result<u64> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    // SAFETY: helObtainHandle() writes a handle on success.
    let error =
        unsafe { hel_sys::helObtainHandle(hel_sys::kHelObtainClockPage as i32, &mut handle) };
    if error != hel_sys::kHelErrNone as hel_sys::HelError {
        return Err(hel::Error::from(error).into());
    }
    // SAFETY: We own the new handle.
    let memory = unsafe { hel::Handle::from_raw(handle) };
    // SAFETY: The page is only read through raw pointers.
    let mapping = unsafe {
        hel::Mapping::<hel_sys::HelClockPage>::new(&memory, None, 0, 4096, hel::MappingFlags::READ)
    }?;
    let page = mapping.address() as *const hel_sys::HelClockPage;
    let (clock_type, shift, factor) = loop {
        // SAFETY: The page stays mapped; the kernel updates it under a seqlock.
        unsafe {
            let before = std::ptr::read_volatile(&raw const (*page).seqlock);
            std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
            let clock_type = std::ptr::read_volatile(&raw const (*page).clockType);
            let shift = std::ptr::read_volatile(&raw const (*page).tickShift);
            let factor = std::ptr::read_volatile(&raw const (*page).tickFactor);
            std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
            let after = std::ptr::read_volatile(&raw const (*page).seqlock);
            if before % 2 == 0 && before == after {
                break (clock_type, shift, factor);
            }
        }
    };
    if clock_type != hel_sys::kHelClockTsc as u32 {
        return Err(Error::NotTsc);
    }
    assert!(
        factor != 0 && (0..64).contains(&shift),
        "thor's clock page has a bad conversion (factor {factor}, shift {shift})"
    );
    // nanoseconds = (factor * ticks) >> shift, hence Hz = 10^9 * 2^shift / factor.
    Ok(((1_000_000_000u128 << shift) / factor as u128) as u64)
}
