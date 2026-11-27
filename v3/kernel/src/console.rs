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
/// One bench protocol line: `CHECK <id> <PASS|FAIL|INFO> <detail>` (see lichen_kcore::check).
#[macro_export]
macro_rules! kcheck {
    ($id:expr, $v:expr, $($a:tt)*) => {{
        let _ = lichen_kcore::check::write_check(&mut $crate::console::Console, $id, $v, format_args!($($a)*));
    }};
}
