use uacpi_sys::uacpi_sleep_state;

use super::runtime::Aml;
use super::{Result, check};

/// A sleep state of the system.
#[derive(Clone, Copy)]
pub enum SleepState {
    /// Soft off.
    S5,
}

impl SleepState {
    fn to_raw(self) -> uacpi_sleep_state {
        match self {
            SleepState::S5 => uacpi_sys::UACPI_SLEEP_STATE_S5,
        }
    }
}

/// Wraps uacpi_prepare_for_sleep_state(), i.e., evaluates _PTS and _SST.
pub fn prepare_for_sleep_state(_aml: Aml, state: SleepState) -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_prepare_for_sleep_state", unsafe {
        uacpi_sys::uacpi_prepare_for_sleep_state(state.to_raw())
    })
}

/// Wraps uacpi_enter_sleep_state(), which only returns if the system did not enter the state.
pub fn enter_sleep_state(_aml: Aml, state: SleepState) -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_enter_sleep_state", unsafe {
        uacpi_sys::uacpi_enter_sleep_state(state.to_raw())
    })
}

/// Wraps uacpi_reboot(), i.e., writes the reset register of the FADT. Only returns on failure.
pub fn reboot() -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_reboot", unsafe { uacpi_sys::uacpi_reboot() })
}
