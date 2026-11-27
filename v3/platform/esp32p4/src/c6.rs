//! C6 bring-up as v2 does it: GPIO13 (the C6 GPIO9 boot strap) high so it boots from its own flash, then GPIO12 (the
//! C6 EN pin) high so it runs. This image does no C6 transport; it only leaves the C6 released.
use crate::gpio;

pub const STRAP_PIN: u32 = 13;
pub const EN_PIN: u32 = 12;

/// Strap first, then EN: the C6 samples the strap when EN rises.
pub fn release() {
    gpio::output(STRAP_PIN, true);
    gpio::output(EN_PIN, true);
}
pub fn strap_level() -> bool {
    gpio::level_in(STRAP_PIN)
}
pub fn en_level() -> bool {
    gpio::level_in(EN_PIN)
}
