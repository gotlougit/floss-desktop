// SPDX-License-Identifier: MIT
mod transport;
use std::{
    collections::VecDeque,
    ffi::c_void,
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    os::linux::net::SocketAddrExt,
    os::{
        fd::AsRawFd,
        unix::{
            fs::{OpenOptionsExt, PermissionsExt},
            net::{SocketAddr, UnixDatagram},
        },
    },
    path::Path,
    time::{Duration, Instant},
};
use transport::{Action, Transport};
unsafe extern "C" {
    fn usb_transport_open(
        cb: unsafe extern "C" fn(*mut c_void, u8, *const u8, i32),
        data: *mut c_void,
    ) -> *mut c_void;
    fn usb_transport_bus(u: *mut c_void) -> i32;
    fn usb_transport_address(u: *mut c_void) -> i32;
    fn usb_transport_bootstrap(index: u32) -> i32;
    fn usb_transport_claim(u: *mut c_void) -> i32;
    fn usb_transport_wbs_packet_size(u: *mut c_void) -> i32;
    fn usb_transport_set_sco(u: *mut c_void, mode: i32) -> i32;
    fn usb_transport_send(u: *mut c_void, kind: u8, data: *const u8, length: i32) -> i32;
    fn usb_transport_pump(u: *mut c_void, ms: i32) -> i32;
    fn usb_transport_close(u: *mut c_void);
    fn transport_poll_fd(fd: i32, ms: i32) -> i32;
    fn transport_install_signals();
    fn transport_stopping() -> i32;
}
#[derive(Default)]
struct Incoming {
    queue: VecDeque<(u8, Vec<u8>)>,
    bytes: usize,
    overflow: bool,
}
unsafe extern "C" fn receive(data: *mut c_void, kind: u8, bytes: *const u8, length: i32) {
    // C invokes this synchronously only during pump/cancel on this same thread.
    // The boxed queue remains allocated until all transfers have been cancelled.
    let incoming = unsafe { &mut *(data as *mut Incoming) };
    if !(0..=4096).contains(&length)
        || incoming.queue.len() >= 128
        || incoming.bytes + length as usize > 262144
    {
        incoming.overflow = true;
        return;
    }
    if length == 0 {
        return;
    }
    let buffer = unsafe { std::slice::from_raw_parts(bytes, length as usize) }.to_vec();
    incoming.bytes += buffer.len();
    incoming.queue.push_back((kind, buffer));
}
struct Usb(*mut c_void);
impl Drop for Usb {
    fn drop(&mut self) {
        unsafe { usb_transport_close(self.0) }
    }
}
fn checked(code: i32, operation: &str) -> io::Result<()> {
    if code < 0 {
        Err(io::Error::other(format!("{operation} failed ({code})")))
    } else {
        Ok(())
    }
}
fn number(path: impl AsRef<Path>) -> Option<i32> {
    fs::read_to_string(path).ok()?.trim().parse().ok()
}
fn physical_index(bus: i32, address: i32) -> io::Result<u32> {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        if let Some(index) = find_physical_index(Path::new("/sys/class/bluetooth"), bus, address)? {
            return Ok(index);
        }
        if Instant::now() >= deadline {
            return Err(io::Error::other("physical btusb adapter did not register"));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}
fn find_physical_index(root: &Path, bus: i32, address: i32) -> io::Result<Option<u32>> {
    let entries = match fs::read_dir(root) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error),
    };
    for item in entries {
        let item = item?;
        let name = item.file_name().to_string_lossy().into_owned();
        let Ok(path) = fs::canonicalize(item.path().join("device")) else {
            continue;
        };
        if path.ancestors().any(|p| {
            number(p.join("busnum")) == Some(bus) && number(p.join("devnum")) == Some(address)
        }) {
            if let Some(index) = name.strip_prefix("hci").and_then(|s| s.parse().ok()) {
                return Ok(Some(index));
            }
        }
    }
    Ok(None)
}
fn write_record(file: &mut File, packet: &[u8]) -> io::Result<()> {
    loop {
        match file.write(packet) {
            Ok(n) if n == packet.len() => return Ok(()),
            Ok(_) => return Err(io::Error::other("short virtual-HCI packet write")),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
}
fn ready() -> io::Result<()> {
    if let Some(path) = std::env::var_os("NOTIFY_SOCKET") {
        let path = path.to_string_lossy();
        let addr = if let Some(name) = path.strip_prefix('@') {
            SocketAddr::from_abstract_name(name.as_bytes())?
        } else {
            SocketAddr::from_pathname(path.as_ref())?
        };
        UnixDatagram::unbound()?
            .send_to_addr(b"READY=1\nSTATUS=USB HCI/SCO transport ready", &addr)?;
    }
    Ok(())
}
fn run() -> io::Result<()> {
    if std::env::args().len() != 1 {
        return Err(io::Error::other(
            "floss-usb takes no arguments; supports one Realtek 0bda:c123 radio",
        ));
    }
    unsafe { transport_install_signals() };
    let mut incoming = Box::<Incoming>::default();
    let ptr =
        unsafe { usb_transport_open(receive, (&mut *incoming) as *mut Incoming as *mut c_void) };
    if ptr.is_null() {
        return Err(io::Error::other("cannot open supported USB controller"));
    }
    let usb = Usb(ptr); // Drops before the callback queue, including on errors.
    let (bus, address) = unsafe { (usb_transport_bus(ptr), usb_transport_address(ptr)) };
    let index = physical_index(bus, address)?;
    checked(
        unsafe { usb_transport_bootstrap(index) },
        "kernel firmware initialization (controller must be unblocked and unused)",
    )?;
    checked(unsafe { usb_transport_claim(ptr) }, "exclusive USB claim")?;
    let mut vhci = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(0x800 | 0x80000)
        .open("/dev/vhci")?;
    write_record(&mut vhci, &[0xff, 0])?;
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut buffer = [0u8; 4097];
    let virtual_index = loop {
        if unsafe { transport_poll_fd(vhci.as_raw_fd(), 10) } > 0 {
            let n = vhci.read(&mut buffer)?;
            if n != 4 || buffer[..2] != [0xff, 0] {
                return Err(io::Error::other("invalid virtual-HCI registration reply"));
            }
            break u16::from_le_bytes([buffer[2], buffer[3]]);
        }
        if Instant::now() >= deadline {
            return Err(io::Error::other("virtual-HCI registration timed out"));
        }
    };
    if virtual_index != 0 {
        return Err(io::Error::other(format!(
            "expected virtual hci0, got hci{virtual_index}; multiple adapters are unsupported"
        )));
    }
    let size = unsafe { usb_transport_wbs_packet_size(ptr) };
    let runtime = Path::new("/run/floss-usb");
    let pending = runtime.join("sco-packet-size.new");
    fs::write(&pending, format!("{size}\n"))?;
    fs::set_permissions(&pending, fs::Permissions::from_mode(0o640))?;
    fs::rename(&pending, runtime.join("sco-packet-size"))?;
    eprintln!(
        "floss-usb: USB {bus}:{address}, physical hci{index} -> virtual hci0, mSBC packet size {size}"
    );
    ready()?;
    let mut state = Transport::default();
    let mut pending: VecDeque<Action> = VecDeque::new();
    while unsafe { transport_stopping() } == 0 {
        if incoming.overflow {
            return Err(io::Error::other("bounded USB receive queue overflow"));
        }
        while let Some((kind, data)) = incoming.queue.pop_front() {
            incoming.bytes -= data.len();
            pending.extend(state.usb(kind, &data).map_err(io::Error::other)?);
            if pending.len() > 256 {
                return Err(io::Error::other("bounded HCI action queue overflow"));
            }
        }
        while let Some(action) = pending.pop_front() {
            match &action {
                Action::Host(packet) => write_record(&mut vhci, packet)?,
                Action::Sco(mode) => {
                    checked(
                        unsafe { usb_transport_set_sco(ptr, *mode as i32) },
                        "USB SCO endpoint change",
                    )?;
                    if *mode == 0 {
                        eprintln!(
                            "floss-usb: SCO stopped; RX {} bytes, TX {} bytes",
                            state.sco_rx_bytes, state.sco_tx_bytes
                        );
                    }
                }
                Action::Usb(packet) => {
                    let rc = unsafe {
                        usb_transport_send(
                            ptr,
                            packet[0],
                            packet[1..].as_ptr(),
                            (packet.len() - 1) as i32,
                        )
                    };
                    if rc == -105 {
                        pending.push_front(action);
                        break;
                    } // bounded transfer pool backpressure
                    checked(rc, "USB transmit")?;
                }
            }
        }
        if pending.is_empty() {
            for _ in 0..32 {
                match vhci.read(&mut buffer) {
                    Ok(0) => return Err(io::Error::other("virtual HCI closed")),
                    Ok(n) => {
                        pending.extend(state.host(&buffer[..n]).map_err(io::Error::other)?);
                        break;
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(e) => return Err(e),
                }
            }
        }
        checked(unsafe { usb_transport_pump(ptr, 1) }, "USB events")?;
    }
    // Unregister virtual HCI before restoring the physical kernel driver.
    drop(vhci);
    drop(usb);
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("floss-usb: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn physical_controller_mapping_ignores_virtual_and_other_usb_devices() {
        let temp = std::env::temp_dir().join(format!("floss-usb-sysfs-{}", std::process::id()));
        fs::create_dir(&temp).unwrap();
        let class = temp.join("class");
        assert_eq!(find_physical_index(&class, 3, 2).unwrap(), None);
        for (index, physical, bus, address) in [(0, false, 0, 0), (1, true, 3, 2), (2, true, 3, 9)]
        {
            let device = temp.join(format!("device-{index}"));
            fs::create_dir_all(device.join("interface")).unwrap();
            if physical {
                fs::write(device.join("busnum"), bus.to_string()).unwrap();
                fs::write(device.join("devnum"), address.to_string()).unwrap();
            }
            let hci = class.join(format!("hci{index}"));
            fs::create_dir_all(&hci).unwrap();
            std::os::unix::fs::symlink(device.join("interface"), hci.join("device")).unwrap();
        }
        assert_eq!(find_physical_index(&class, 3, 2).unwrap(), Some(1));
        assert_eq!(find_physical_index(&class, 3, 9).unwrap(), Some(2));
        assert_eq!(find_physical_index(&class, 4, 2).unwrap(), None);
        fs::remove_dir_all(temp).unwrap();
    }
}
