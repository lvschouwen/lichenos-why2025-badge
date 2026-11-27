//! The one I/O the stage does: read flash. There is deliberately no write or erase in this trait.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FlashError;

pub trait FlashRead {
    /// Fill `buf` from flash at byte `offset`. Any length and alignment; the implementation handles the hardware's.
    fn read(&mut self, offset: u32, buf: &mut [u8]) -> Result<(), FlashError>;
}
