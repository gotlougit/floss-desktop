// SPDX-License-Identifier: MIT
mod transport;
mod usb;
use std::{
    cell::RefCell,
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
#[derive(Default)]
struct Incoming {
    queue: VecDeque<(u8, Vec<u8>)>,
    bytes: usize,
    overflow: bool,
}
fn receive(data: *mut c_void, kind: u8, buffer: &[u8]) {
    // libusb invokes this synchronously only during pump/cancel on this thread.
    // The boxed queue remains allocated until all transfers have been cancelled.
    let mut incoming = unsafe { &*(data as *const RefCell<Incoming>) }.borrow_mut();
    if buffer.len() > 4096 || incoming.queue.len() >= 128 || incoming.bytes + buffer.len() > 262144
    {
        incoming.overflow = true;
        return;
    }
    if buffer.is_empty() {
        return;
    }
    let buffer = buffer.to_vec();
    incoming.bytes += buffer.len();
    incoming.queue.push_back((kind, buffer));
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
// Drain a bounded burst before waiting for USB events. SCO replies often arrive
// together after one isochronous receive transfer; one packet per millisecond
// artificially spreads that burst and can leave the USB transmit endpoint idle.
fn queue_host(
    reader: &mut impl Read,
    state: &mut Transport,
    pending: &mut VecDeque<Action>,
) -> io::Result<()> {
    let mut buffer = [0u8; 4097];
    for _ in 0..32 {
        match reader.read(&mut buffer) {
            Ok(0) => return Err(io::Error::other("virtual HCI closed")),
            Ok(n) => pending.extend(state.host(&buffer[..n]).map_err(io::Error::other)?),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}
fn wait_registration(
    reader: &mut impl Read,
    mut poll: impl FnMut() -> i32,
    mut stopping: impl FnMut() -> bool,
    timeout: Duration,
) -> io::Result<u16> {
    let deadline = Instant::now() + timeout;
    let mut buffer = [0u8; 4097];
    loop {
        if stopping() {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "virtual HCI registration cancelled",
            ));
        }
        if Instant::now() >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "virtual-HCI registration timed out",
            ));
        }
        let result = poll();
        if result < 0 {
            return Err(io::Error::from_raw_os_error(-result));
        }
        if result == 0 {
            continue;
        }
        match reader.read(&mut buffer) {
            Ok(4) if buffer[..2] == [0xff, 0] => {
                return Ok(u16::from_le_bytes([buffer[2], buffer[3]]));
            }
            Ok(_) => return Err(io::Error::other("invalid virtual-HCI registration reply")),
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::Interrupted | io::ErrorKind::WouldBlock
                ) =>
            {
                continue;
            }
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
    usb::install_signals();
    // Shared ownership of the cell keeps the callback pointer valid across main
    // loop accesses. Drop USB first so no callback can outlive this allocation.
    let incoming = Box::new(RefCell::new(Incoming::default()));
    let mut usb = usb::Usb::open(receive, std::ptr::from_ref(&*incoming) as *mut c_void)?;
    let (bus, address) = (usb.bus(), usb.address());
    let index = physical_index(bus, address)?;
    checked(
        usb::bootstrap(index),
        "kernel firmware initialization (controller must be unblocked and unused)",
    )?;
    checked(usb.claim(), "exclusive USB claim")?;
    let mut vhci = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(0x800 | 0x80000)
        .open("/dev/vhci")?;
    write_record(&mut vhci, &[0xff, 0])?;
    let fd = vhci.as_raw_fd();
    let virtual_index = wait_registration(
        &mut vhci,
        || usb::poll_fd(fd, 10),
        usb::stopping,
        Duration::from_secs(5),
    )?;
    if virtual_index != 0 {
        return Err(io::Error::other(format!(
            "expected virtual hci0, got hci{virtual_index}; multiple adapters are unsupported"
        )));
    }
    let size = usb.wbs_packet_size();
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
    while !usb::stopping() {
        {
            // End this borrow before USB calls, which may invoke receive().
            let mut incoming = incoming.borrow_mut();
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
        }
        let mut blocked = false;
        while let Some(action) = pending.pop_front() {
            match &action {
                Action::Host(packet) => write_record(&mut vhci, packet)?,
                Action::Sco(mode) => {
                    checked(usb.set_sco(*mode as i32), "USB SCO endpoint change")?;
                    if *mode == 0 {
                        eprintln!(
                            "floss-usb: SCO stopped; RX {} bytes, TX {} bytes",
                            state.sco_rx_bytes, state.sco_tx_bytes
                        );
                    }
                }
                Action::Usb(packet) => {
                    let rc = usb.send(packet[0], &packet[1..]);
                    if rc == -105 {
                        pending.push_front(action);
                        blocked = true;
                        break;
                    } // bounded transfer pool backpressure
                    checked(rc, "USB transmit")?;
                }
            }
        }
        if pending.is_empty() {
            queue_host(&mut vhci, &mut state, &mut pending)?;
        }
        // Do not sleep on work we can submit now. Still wait on pool exhaustion
        // so backpressure cannot turn into a busy-spin.
        let wait_ms = if blocked || pending.is_empty() { 1 } else { 0 };
        checked(usb.pump(wait_ms), "USB events")?;
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
    fn receive_queue_can_be_drained_between_callbacks_and_stays_bounded() {
        let incoming = Box::new(RefCell::new(Incoming::default()));
        let data = std::ptr::from_ref(&*incoming) as *mut c_void;
        receive(data, 2, &[1, 2, 3]);
        {
            let mut queue = incoming.borrow_mut();
            assert_eq!(queue.queue.pop_front(), Some((2, vec![1, 2, 3])));
            queue.bytes = 0;
        }
        receive(data, 4, &[]);
        for _ in 0..128 {
            receive(data, 3, &[7]);
        }
        receive(data, 3, &[8]);
        let queue = incoming.borrow();
        assert_eq!(queue.queue.len(), 128);
        assert_eq!(queue.bytes, 128);
        assert!(queue.overflow);
    }
    #[test]
    fn registration_reports_poll_errors_shutdown_timeout_and_bad_records() {
        let mut bytes = io::Cursor::new(vec![0xff, 0, 7, 0]);
        let limit = Duration::from_secs(1);
        let error = wait_registration(&mut bytes, || -5, || false, limit).unwrap_err();
        assert_eq!(error.raw_os_error(), Some(5));
        assert_eq!(bytes.position(), 0);
        assert_eq!(
            wait_registration(&mut bytes, || panic!("poll after shutdown"), || true, limit)
                .unwrap_err()
                .kind(),
            io::ErrorKind::Interrupted
        );
        assert_eq!(
            wait_registration(
                &mut bytes,
                || panic!("poll after deadline"),
                || false,
                Duration::ZERO
            )
            .unwrap_err()
            .kind(),
            io::ErrorKind::TimedOut
        );
        assert_eq!(
            wait_registration(&mut bytes, || 1, || false, limit).unwrap(),
            7
        );
        let mut malformed = io::Cursor::new(vec![0xff, 0, 1]);
        assert!(wait_registration(&mut malformed, || 1, || false, limit).is_err());
    }
    #[test]
    fn registration_retries_transient_read_and_observes_stop_after_poll() {
        struct TransientReader(bool);
        impl Read for TransientReader {
            fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
                if !self.0 {
                    self.0 = true;
                    return Err(io::ErrorKind::WouldBlock.into());
                }
                out[..4].copy_from_slice(&[0xff, 0, 0, 0]);
                Ok(4)
            }
        }
        assert_eq!(
            wait_registration(
                &mut TransientReader(false),
                || 1,
                || false,
                Duration::from_secs(1)
            )
            .unwrap(),
            0
        );
        let stopped = std::cell::Cell::new(false);
        assert_eq!(
            wait_registration(
                &mut io::empty(),
                || {
                    stopped.set(true);
                    0
                },
                || stopped.get(),
                Duration::from_secs(1)
            )
            .unwrap_err()
            .kind(),
            io::ErrorKind::Interrupted
        );
    }
    struct HostBurst {
        remaining: usize,
    }
    impl Read for HostBurst {
        fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
            if self.remaining == 0 {
                return Err(io::ErrorKind::WouldBlock.into());
            }
            self.remaining -= 1;
            out[..4].copy_from_slice(&[1, 1, 16, 0]); // Read Local Version command.
            Ok(4)
        }
    }
    #[test]
    fn host_burst_is_drained_before_usb_wait_with_bounded_fairness() {
        let mut state = Transport::default();
        let mut pending = VecDeque::new();
        let mut burst = HostBurst { remaining: 40 };
        queue_host(&mut burst, &mut state, &mut pending).unwrap();
        assert_eq!(pending.len(), 32);
        assert_eq!(burst.remaining, 8);
        pending.clear();
        queue_host(&mut burst, &mut state, &mut pending).unwrap();
        assert_eq!(pending.len(), 8);
        assert_eq!(burst.remaining, 0);
    }
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
