pub struct Console;
impl core::fmt::Write for Console {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                crate::platform::putc(b'\r');
            }
            crate::platform::putc(b);
        }
        Ok(())
    }
}
#[macro_export]
macro_rules! kprint {
    ($($a:tt)*) => {{ let _ = core::fmt::Write::write_fmt(&mut $crate::console::Console, format_args!($($a)*)); }};
}
#[macro_export]
macro_rules! kprintln {
    ($($a:tt)*) => {{ $crate::kprint!($($a)*); $crate::kprint!("\n"); }};
}
