mod mock_libc {
    pub use std::ffi::{c_char, c_int, c_uint, c_ulong, c_ushort, c_void};
    use std::{cell::RefCell, ffi::CStr, fs::OpenOptions, os::fd::IntoRawFd};
    #[repr(C)]
    pub struct timeval { pub tv_sec: i64, pub tv_usec: i64 }
    pub const O_RDWR: i32 = 2;
    pub const O_CLOEXEC: i32 = 0x80000;
    extern "C" { fn __errno_location() -> *mut i32; }
    pub fn errno(value: i32) { unsafe { *__errno_location() = value; } }
    #[derive(Default)]
    pub struct State {
        pub fail_open: bool, pub opens: usize, pub fail_ioctl: usize,
        pub calls: usize, pub short_write: bool, pub interrupt: bool,
        pub fd: Option<i32>, pub destroys: usize, pub phys: String,
        pub writes: Vec<Vec<u8>>,
    }
    thread_local! { pub static STATE: RefCell<State> = RefCell::new(State::default()); }
    pub unsafe fn open(_path: *const c_char, flags: i32) -> i32 {
        assert_eq!(flags, O_RDWR | O_CLOEXEC);
        STATE.with(|s| {
            let mut s = s.borrow_mut(); s.opens += 1;
            if s.fail_open { errno(if s.opens == 1 { 13 } else { 2 }); return -1; }
            let fd = OpenOptions::new().read(true).write(true).open("/dev/null").unwrap().into_raw_fd();
            s.fd = Some(fd); fd
        })
    }
    pub trait Arg { fn value(self) -> usize; }
    impl Arg for i32 { fn value(self) -> usize { self as usize } }
    impl Arg for u32 { fn value(self) -> usize { self as usize } }
    impl Arg for *const c_char { fn value(self) -> usize { self as usize } }
    pub unsafe fn ioctl<T: Arg>(fd: i32, request: u64, arg: T) -> i32 {
        let arg = arg.value();
        STATE.with(|s| {
            let mut s = s.borrow_mut();
            assert_eq!(Some(fd), s.fd);
            s.calls += 1;
            if s.calls == s.fail_ioctl { errno(5); return -1; }
            if request & 0xff == 108 {
                assert_eq!(request, 0x4008556c); // Linux x86_64 UI_SET_PHYS(char *)
                s.phys = CStr::from_ptr(arg as *const c_char).to_str().unwrap().into();
            }
            if request & 0xff == 2 { s.destroys += 1; }
            0
        })
    }
    pub unsafe fn write(fd: i32, data: *const c_void, size: usize) -> isize {
        STATE.with(|s| {
            let mut s = s.borrow_mut(); assert_eq!(Some(fd), s.fd);
            if s.interrupt { s.interrupt = false; errno(4); return -1; }
            if s.short_write { return size as isize - 1; }
            s.writes.push(std::slice::from_raw_parts(data.cast::<u8>(), size).into());
            size as isize
        })
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use mock_libc::{STATE, State};
    const ADDR: &str = "80:99:E7:59:66:9E";
    fn reset() { STATE.with(|s| *s.borrow_mut() = State::default()); }
    fn closed() {
        STATE.with(|s| {
            let fd = s.borrow().fd.unwrap();
            assert!(!std::path::Path::new(&format!("/proc/self/fd/{fd}")).exists());
        });
    }
    #[test] fn open_error_preserves_permission_failure() {
        reset(); STATE.with(|s| s.borrow_mut().fail_open = true);
        let mut d = UInputDev::default();
        assert!(d.init("Sony".into(), ADDR.into()).unwrap_err().contains("Permission denied"));
        assert!(d.fd.is_none());
    }
    #[test] fn each_setup_ioctl_failure_closes_fd() {
        // 4 event bits, physical address, 7 keys, create.
        for fail in 1..=13 {
            reset(); STATE.with(|s| s.borrow_mut().fail_ioctl = fail);
            let mut d = UInputDev::default();
            assert!(d.init("Sony".into(), ADDR.into()).is_err(), "ioctl {fail}");
            assert!(d.fd.is_none()); closed(); drop(d);
            STATE.with(|s| assert_eq!(s.borrow().destroys, 0));
        }
    }
    #[test] fn short_setup_write_closes_fd() {
        reset(); STATE.with(|s| s.borrow_mut().short_write = true);
        let mut d = UInputDev::default();
        assert!(d.init("Sony".into(), ADDR.into()).unwrap_err().contains("Short")); closed();
    }
    #[test] fn unicode_name_is_utf8_and_nul_terminated() {
        reset(); let mut d = UInputDev::default();
        d.init("🎧日本語".repeat(30), ADDR.into()).unwrap();
        let bytes: Vec<u8> = d.device.name.iter().map(|b| *b as u8).collect();
        let end = bytes.iter().position(|b| *b == 0).unwrap();
        assert!(std::str::from_utf8(&bytes[..end]).unwrap().ends_with(" (AVRCP)"));
        STATE.with(|s| assert_eq!(s.borrow().phys, ADDR));
    }
    #[test] fn close_is_idempotent_even_after_destroy_failure() {
        reset(); let mut d = UInputDev::default(); d.init("Sony".into(), ADDR.into()).unwrap();
        STATE.with(|s| s.borrow_mut().fail_ioctl = 14);
        d.close(); closed(); d.close(); drop(d);
        STATE.with(|s| assert_eq!(s.borrow().calls, 14));
    }
    #[test] fn drop_does_not_close_a_reused_descriptor() {
        use std::os::fd::AsRawFd;
        reset(); let mut d = UInputDev::default(); d.init("Sony".into(), ADDR.into()).unwrap();
        let old = d.fd.as_ref().unwrap().as_raw_fd(); d.close();
        let replacement = std::fs::File::open("/dev/null").unwrap();
        assert_eq!(old, replacement.as_raw_fd());
        drop(d);
        assert!(replacement.metadata().is_ok());
    }
    #[test] fn invalid_address_opens_nothing() {
        reset(); let mut d = UInputDev::default();
        assert!(d.init("Sony".into(), "bad\0address".into()).is_err());
        STATE.with(|s| assert_eq!(s.borrow().opens, 0));
    }
    #[test] fn repeated_initialization_preserves_original_device() {
        reset(); let mut d = UInputDev::default(); d.init("Sony".into(), ADDR.into()).unwrap();
        assert!(d.init("Other".into(), ADDR.into()).is_err());
        d.send_key(AVC_PLAY, 1).unwrap();
        STATE.with(|s| assert_eq!(s.borrow().opens, 1));
    }
    #[test] fn collection_drop_destroys_each_device_once() {
        reset(); let mut u = UInput::new(); u.create("Sony".into(), ADDR.into()).unwrap();
        drop(u); closed(); STATE.with(|s| assert_eq!(s.borrow().destroys, 1));
    }
    #[test] fn key_events_have_press_release_and_syn() {
        reset(); let mut u = UInput::new(); u.create("Sony".into(), ADDR.into()).unwrap();
        assert!(u.send_key(AVC_PLAY, 1).is_err());
        u.set_active_device(ADDR.into());
        STATE.with(|s| { s.borrow_mut().writes.clear(); s.borrow_mut().interrupt = true; });
        for key in KEY_MAP { u.send_key(key.avc, 1).unwrap(); u.send_key(key.avc, 0).unwrap(); }
        STATE.with(|s| {
            let s = s.borrow(); assert_eq!(s.writes.len(), 28);
            for pair in s.writes.chunks(2) {
                assert_eq!(u16::from_ne_bytes(pair[0][16..18].try_into().unwrap()), 1);
                assert_eq!(u16::from_ne_bytes(pair[1][16..18].try_into().unwrap()), 0);
            }
            assert_eq!(i32::from_ne_bytes(s.writes[0][20..24].try_into().unwrap()), 1);
            assert_eq!(i32::from_ne_bytes(s.writes[2][20..24].try_into().unwrap()), 0);
        });
        assert!(u.send_key(0xff, 1).is_err()); assert!(u.send_key(AVC_PLAY, 2).is_err());
        STATE.with(|s| s.borrow_mut().short_write = true);
        assert!(u.send_key(AVC_PLAY, 1).unwrap_err().contains("Short"));
        u.close(ADDR.into()); closed(); assert!(u.send_key(AVC_PLAY, 1).is_err());
    }
}
