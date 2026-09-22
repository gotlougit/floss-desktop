// SPDX-License-Identifier: MIT
//! Small libusb/OS boundary and asynchronous USB transfer owner.
use std::{
    cell::UnsafeCell,
    ffi::{c_int, c_uchar, c_uint, c_void},
    io,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr::{self, addr_of_mut},
    sync::atomic::{AtomicBool, Ordering},
};

const TRANSFERS: usize = 72;
const RX_ISO_PACKETS: i32 = 10;
const EIO: i32 = 5;
const ENOMEM: i32 = 12;
const EBUSY: i32 = 16;
const EINVAL: i32 = 22;
const ENOTSUP: i32 = 95;
const ENOBUFS: i32 = 105;
const ENOTCONN: i32 = 107;
const ETIMEDOUT: i32 = 110;
const ECANCELED: i32 = 125;
const LIBUSB_ERROR_INTERRUPTED: i32 = -10;
const COMPLETED: i32 = 0;
const ERROR: i32 = 1;
const TIMED_OUT: i32 = 2;
const CANCELLED: i32 = 3;
const TYPE_CONTROL: u8 = 0;
const TYPE_ISO: u8 = 1;
const TYPE_BULK: u8 = 2;
const TYPE_INTERRUPT: u8 = 3;

#[repr(C)]
struct Context(c_void);
#[repr(C)]
struct Device(c_void);
#[repr(C)]
struct DeviceHandle(c_void);
#[repr(C)]
struct IsoPacket {
    length: c_uint,
    actual_length: c_uint,
    status: c_int,
}
#[repr(C)]
struct LibusbTransfer {
    dev_handle: *mut DeviceHandle,
    flags: c_uchar,
    endpoint: c_uchar,
    transfer_type: c_uchar,
    _padding: c_uchar,
    timeout: c_uint,
    status: c_int,
    length: c_int,
    actual_length: c_int,
    callback: Option<unsafe extern "C" fn(*mut LibusbTransfer)>,
    user_data: *mut c_void,
    buffer: *mut u8,
    num_iso_packets: c_int,
    iso_packet_desc: [IsoPacket; 0],
}
#[repr(C)]
#[derive(Default)]
struct DeviceDescriptor {
    length: u8,
    descriptor_type: u8,
    usb: u16,
    class: u8,
    subclass: u8,
    protocol: u8,
    max_packet: u8,
    vendor: u16,
    product: u16,
    device: u16,
    manufacturer: u8,
    product_string: u8,
    serial: u8,
    configurations: u8,
}
#[repr(C)]
struct EndpointDescriptor {
    length: u8,
    descriptor_type: u8,
    address: u8,
    attributes: u8,
    max_packet_size: u16,
    interval: u8,
    refresh: u8,
    sync_address: u8,
    extra: *const u8,
    extra_length: c_int,
}
#[repr(C)]
struct InterfaceDescriptor {
    length: u8,
    descriptor_type: u8,
    number: u8,
    alternate: u8,
    endpoint_count: u8,
    class: u8,
    subclass: u8,
    protocol: u8,
    interface_string: u8,
    endpoints: *const EndpointDescriptor,
    extra: *const u8,
    extra_length: c_int,
}
#[repr(C)]
struct Interface {
    alternates: *const InterfaceDescriptor,
    alternate_count: c_int,
}
#[repr(C)]
struct ConfigDescriptor {
    length: u8,
    descriptor_type: u8,
    total_length: u16,
    interface_count: u8,
    configuration: u8,
    configuration_string: u8,
    attributes: u8,
    max_power: u8,
    interfaces: *const Interface,
    extra: *const u8,
    extra_length: c_int,
}
#[repr(C)]
struct Timeval {
    sec: isize,
    usec: isize,
}
#[repr(C)]
struct Timespec {
    sec: isize,
    nsec: isize,
}
#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

unsafe extern "C" {
    fn libusb_init(context: *mut *mut Context) -> c_int;
    fn libusb_exit(context: *mut Context);
    fn libusb_get_device_list(context: *mut Context, list: *mut *mut *mut Device) -> isize;
    fn libusb_free_device_list(list: *mut *mut Device, unref: c_int);
    fn libusb_get_device_descriptor(
        device: *mut Device,
        descriptor: *mut DeviceDescriptor,
    ) -> c_int;
    fn libusb_open(device: *mut Device, handle: *mut *mut DeviceHandle) -> c_int;
    fn libusb_close(handle: *mut DeviceHandle);
    fn libusb_get_bus_number(device: *mut Device) -> u8;
    fn libusb_get_device_address(device: *mut Device) -> u8;
    fn libusb_get_device(handle: *mut DeviceHandle) -> *mut Device;
    fn libusb_get_active_config_descriptor(
        device: *mut Device,
        config: *mut *mut ConfigDescriptor,
    ) -> c_int;
    fn libusb_free_config_descriptor(config: *mut ConfigDescriptor);
    fn libusb_kernel_driver_active(handle: *mut DeviceHandle, interface: c_int) -> c_int;
    fn libusb_detach_kernel_driver(handle: *mut DeviceHandle, interface: c_int) -> c_int;
    fn libusb_attach_kernel_driver(handle: *mut DeviceHandle, interface: c_int) -> c_int;
    fn libusb_claim_interface(handle: *mut DeviceHandle, interface: c_int) -> c_int;
    fn libusb_release_interface(handle: *mut DeviceHandle, interface: c_int) -> c_int;
    fn libusb_set_interface_alt_setting(
        handle: *mut DeviceHandle,
        interface: c_int,
        alternate: c_int,
    ) -> c_int;
    fn libusb_alloc_transfer(iso_packets: c_int) -> *mut LibusbTransfer;
    fn libusb_free_transfer(transfer: *mut LibusbTransfer);
    fn libusb_submit_transfer(transfer: *mut LibusbTransfer) -> c_int;
    fn libusb_cancel_transfer(transfer: *mut LibusbTransfer) -> c_int;
    fn libusb_handle_events_timeout_completed(
        context: *mut Context,
        timeout: *mut Timeval,
        completed: *mut c_int,
    ) -> c_int;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn clock_gettime(clock: c_int, time: *mut Timespec) -> c_int;
    fn __errno_location() -> *mut c_int;
    fn poll(fds: *mut PollFd, count: usize, timeout: c_int) -> c_int;
    fn signal(signal: c_int, handler: usize) -> usize;
    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn bind(fd: c_int, address: *const c_void, length: c_uint) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn _exit(status: c_int) -> !;
}

#[derive(Clone, Copy)]
struct Ops {
    alloc: unsafe fn(i32) -> *mut LibusbTransfer,
    free_transfer: unsafe fn(*mut LibusbTransfer),
    alloc_buffer: unsafe fn(usize) -> *mut u8,
    free_buffer: unsafe fn(*mut u8),
    submit: unsafe fn(*mut LibusbTransfer) -> i32,
    cancel: unsafe fn(*mut LibusbTransfer) -> i32,
    events: unsafe fn(*mut Context, i32) -> i32,
    now_ms: unsafe fn() -> Result<i64, i32>,
    set_alt: unsafe fn(*mut DeviceHandle, i32) -> i32,
    descriptors: unsafe fn(*mut DeviceHandle, i32) -> Result<Endpoints, i32>,
}
unsafe fn native_alloc(n: i32) -> *mut LibusbTransfer {
    unsafe { libusb_alloc_transfer(n) }
}
unsafe fn native_free_transfer(t: *mut LibusbTransfer) {
    unsafe { libusb_free_transfer(t) }
}
unsafe fn native_alloc_buffer(n: usize) -> *mut u8 {
    unsafe { calloc(1, n).cast() }
}
unsafe fn native_free_buffer(p: *mut u8) {
    unsafe { free(p.cast()) }
}
unsafe fn native_submit(t: *mut LibusbTransfer) -> i32 {
    unsafe { libusb_submit_transfer(t) }
}
unsafe fn native_cancel(t: *mut LibusbTransfer) -> i32 {
    unsafe { libusb_cancel_transfer(t) }
}
unsafe fn native_events(c: *mut Context, ms: i32) -> i32 {
    let mut timeout = Timeval {
        sec: (ms / 1000) as isize,
        usec: ((ms % 1000) * 1000) as isize,
    };
    unsafe { libusb_handle_events_timeout_completed(c, &mut timeout, ptr::null_mut()) }
}
unsafe fn native_now_ms() -> Result<i64, i32> {
    let mut now = Timespec { sec: 0, nsec: 0 };
    if unsafe { clock_gettime(1, &mut now) } < 0 {
        Err(-unsafe { *__errno_location() })
    } else {
        Ok(now.sec as i64 * 1000 + now.nsec as i64 / 1_000_000)
    }
}
unsafe fn native_set_alt(h: *mut DeviceHandle, alt: i32) -> i32 {
    unsafe { libusb_set_interface_alt_setting(h, 1, alt) }
}
unsafe fn native_descriptors(handle: *mut DeviceHandle, wanted: i32) -> Result<Endpoints, i32> {
    let mut config = ptr::null_mut();
    if unsafe { libusb_get_active_config_descriptor(libusb_get_device(handle), &mut config) } != 0 {
        return Err(-EIO);
    }
    struct ConfigGuard(*mut ConfigDescriptor);
    impl Drop for ConfigGuard {
        fn drop(&mut self) {
            unsafe { libusb_free_config_descriptor(self.0) }
        }
    }
    let _guard = ConfigGuard(config);
    // SAFETY: libusb owns all descriptor arrays until the guard frees config.
    unsafe { parse_descriptors(config, wanted) }
}

// SAFETY: non-null arrays must point to their declared number of initialized
// libusb descriptors and remain alive for this call. Empty/null is accepted.
unsafe fn parse_descriptors(
    config: *const ConfigDescriptor,
    wanted: i32,
) -> Result<Endpoints, i32> {
    if config.is_null() {
        return Err(-EIO);
    }
    let mut owned = Vec::new();
    let interface_count = unsafe { (*config).interface_count as usize };
    if interface_count != 0 && unsafe { (*config).interfaces.is_null() } {
        return Err(-EIO);
    }
    let interfaces = if interface_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts((*config).interfaces, interface_count) }
    };
    for interface in interfaces {
        if interface.alternate_count < 0
            || (interface.alternate_count != 0 && interface.alternates.is_null())
        {
            return Err(-EIO);
        }
        let alts = if interface.alternate_count == 0 {
            &[]
        } else {
            unsafe {
                std::slice::from_raw_parts(interface.alternates, interface.alternate_count as usize)
            }
        };
        for alt in alts {
            if alt.endpoint_count != 0 && alt.endpoints.is_null() {
                return Err(-EIO);
            }
            let native_eps = if alt.endpoint_count == 0 {
                &[]
            } else {
                unsafe { std::slice::from_raw_parts(alt.endpoints, alt.endpoint_count as usize) }
            };
            owned.push(OwnedAlternate {
                number: alt.number,
                alternate: alt.alternate,
                class: alt.class,
                subclass: alt.subclass,
                protocol: alt.protocol,
                endpoints: native_eps
                    .iter()
                    .map(|e| Endpoint {
                        address: e.address,
                        attributes: e.attributes,
                        size: e.max_packet_size,
                    })
                    .collect(),
            });
        }
    }
    let alternates: Vec<_> = owned
        .iter()
        .map(|a| Alternate {
            number: a.number,
            alternate: a.alternate,
            class: a.class,
            subclass: a.subclass,
            protocol: a.protocol,
            endpoints: &a.endpoints,
        })
        .collect();
    discover(&alternates, wanted)
}
const NATIVE: Ops = Ops {
    alloc: native_alloc,
    free_transfer: native_free_transfer,
    alloc_buffer: native_alloc_buffer,
    free_buffer: native_free_buffer,
    submit: native_submit,
    cancel: native_cancel,
    events: native_events,
    now_ms: native_now_ms,
    set_alt: native_set_alt,
    descriptors: native_descriptors,
};

#[derive(Default, Clone, Copy)]
struct Endpoints {
    interrupt_in: u8,
    bulk_in: u8,
    bulk_out: u8,
    iso_in: u8,
    iso_out: u8,
    iso_size: i32,
    wbs_alt: i32,
    wbs_size: i32,
}
#[derive(Clone, Copy)]
struct Endpoint {
    address: u8,
    attributes: u8,
    size: u16,
}
#[derive(Clone, Copy)]
struct Alternate<'a> {
    number: u8,
    alternate: u8,
    class: u8,
    subclass: u8,
    protocol: u8,
    endpoints: &'a [Endpoint],
}
struct OwnedAlternate {
    number: u8,
    alternate: u8,
    class: u8,
    subclass: u8,
    protocol: u8,
    endpoints: Vec<Endpoint>,
}
fn discover(alternates: &[Alternate<'_>], wanted: i32) -> Result<Endpoints, i32> {
    let mut result = Endpoints::default();
    let mut found = false;
    for alt in alternates
        .iter()
        .filter(|a| a.class == 0xe0 && a.subclass == 1 && a.protocol == 1)
    {
        let (mut input, mut output, mut input_size, mut output_size) = (0, 0, 0, 0);
        for ep in alt.endpoints {
            let kind = ep.attributes & 3;
            if alt.number == 0 && alt.alternate == 0 {
                match (kind, ep.address & 0x80 != 0) {
                    (3, true) => result.interrupt_in = ep.address,
                    (2, true) => result.bulk_in = ep.address,
                    (2, false) => result.bulk_out = ep.address,
                    _ => {}
                }
            }
            if alt.number == 1 && kind == 1 {
                let size = i32::from(ep.size & 0x7ff);
                if ep.address & 0x80 != 0 {
                    input = ep.address;
                    input_size = size;
                } else {
                    output = ep.address;
                    output_size = size;
                }
            }
        }
        if alt.number == 1
            && input != 0
            && output != 0
            && input_size == output_size
            && input_size > 0
        {
            if alt.alternate == 1 && result.wbs_alt == 0 {
                result.wbs_alt = 1;
                result.wbs_size = 24;
            }
            if alt.alternate == 6 && input_size >= 63 {
                result.wbs_alt = 6;
                result.wbs_size = 60;
            }
            if i32::from(alt.alternate) == wanted {
                result.iso_in = input;
                result.iso_out = output;
                result.iso_size = input_size;
                found = true;
            }
        }
    }
    if found && result.interrupt_in != 0 && result.bulk_in != 0 && result.bulk_out != 0 {
        Ok(result)
    } else {
        Err(-ENOTSUP)
    }
}

type Receiver = fn(*mut c_void, u8, &[u8]);
struct Slot {
    owner: *mut Inner,
    transfer: *mut LibusbTransfer,
    kind: u8,
    receive: bool,
}
impl Slot {
    const fn empty() -> Self {
        Self {
            owner: ptr::null_mut(),
            transfer: ptr::null_mut(),
            kind: 0,
            receive: false,
        }
    }
}
struct Inner {
    context: *mut Context,
    handle: *mut DeviceHandle,
    receiver: Receiver,
    receiver_data: *mut c_void,
    slots: [Slot; TRANSFERS],
    endpoints: Endpoints,
    alt: i32,
    bus: i32,
    address: i32,
    fatal: i32,
    detached: bool,
    claimed0: bool,
    claimed1: bool,
    stopping: bool,
    stopping_sco: bool,
    alt6_phase: bool,
    iso_rx_errors: u64,
    iso_tx_errors: u64,
    ops: Ops,
}
impl Inner {
    fn active(&self, iso_only: bool) -> usize {
        self.slots
            .iter()
            .filter(|s| !s.transfer.is_null() && (!iso_only || s.kind == 3))
            .count()
    }
}
// All callback state lives at a stable address. Access it through get(), never
// through a whole-Inner mutable reference: libusb retains pointers into slots.
// The helper is single-threaded, and neither Usb nor its raw pointers are Send
// or Sync. No Rust borrow of callback state may span an event-pumping call.
pub struct Usb {
    inner: Box<UnsafeCell<Inner>>,
}

unsafe fn iso(t: *mut LibusbTransfer, i: usize) -> *mut IsoPacket {
    unsafe {
        addr_of_mut!((*t).iso_packet_desc)
            .cast::<IsoPacket>()
            .add(i)
    }
}
unsafe fn free_slot(slot: *mut Slot) {
    let transfer = unsafe { (*slot).transfer };
    if transfer.is_null() {
        return;
    }
    let owner = unsafe { (*slot).owner };
    unsafe {
        ((*owner).ops.free_buffer)((*transfer).buffer);
        ((*owner).ops.free_transfer)(transfer);
        (*slot).transfer = ptr::null_mut();
    }
}
unsafe extern "C" fn completed(transfer: *mut LibusbTransfer) {
    // Receiver panics are handled separately. An unexpected panic here leaves
    // ownership uncertain; do not unwind into C or touch a possibly freed slot.
    if catch_unwind(AssertUnwindSafe(|| unsafe { completed_inner(transfer) })).is_err() {
        std::process::abort();
    }
}
unsafe fn completed_inner(t: *mut LibusbTransfer) {
    let slot = unsafe { (*t).user_data.cast::<Slot>() };
    if slot.is_null() {
        return;
    }
    let owner = unsafe { (*slot).owner };
    let stopping = unsafe { (*owner).stopping || ((*slot).kind == 3 && (*owner).stopping_sco) };
    let status = unsafe { (*t).status };
    let iso_loss = unsafe { (*slot).kind == 3 } && (status == ERROR || status == TIMED_OUT);
    if iso_loss && !stopping {
        unsafe {
            if (*slot).receive {
                (*owner).iso_rx_errors += 1
            } else {
                (*owner).iso_tx_errors += 1
            }
        }
    }
    if status == CANCELLED && !stopping {
        unsafe { (*owner).fatal = -ECANCELED }
    }
    if status != COMPLETED && status != CANCELLED && !iso_loss {
        unsafe { (*owner).fatal = -EIO }
    }
    if status == COMPLETED && !stopping {
        if unsafe { (*slot).receive } {
            if unsafe { (*slot).kind == 3 } {
                for i in 0..unsafe { (*t).num_iso_packets.max(0) as usize } {
                    let packet = unsafe { &*iso(t, i) };
                    if packet.status != COMPLETED {
                        unsafe { (*owner).iso_rx_errors += 1 };
                        continue;
                    }
                    if packet.actual_length != 0 {
                        let offset: usize = (0..i)
                            .map(|j| unsafe { (*iso(t, j)).length as usize })
                            .sum();
                        let bytes = unsafe {
                            std::slice::from_raw_parts(
                                (*t).buffer.add(offset),
                                packet.actual_length as usize,
                            )
                        };
                        let receiver = unsafe { (*owner).receiver };
                        let data = unsafe { (*owner).receiver_data };
                        if catch_unwind(AssertUnwindSafe(|| receiver(data, 3, bytes))).is_err() {
                            unsafe { (*owner).fatal = -EIO };
                        }
                    }
                }
            } else if unsafe { (*t).actual_length > 0 } {
                let bytes =
                    unsafe { std::slice::from_raw_parts((*t).buffer, (*t).actual_length as usize) };
                let receiver = unsafe { (*owner).receiver };
                let data = unsafe { (*owner).receiver_data };
                let kind = unsafe { (*slot).kind };
                if catch_unwind(AssertUnwindSafe(|| receiver(data, kind, bytes))).is_err() {
                    unsafe { (*owner).fatal = -EIO };
                }
            }
        } else if unsafe { (*t).transfer_type == TYPE_ISO } {
            for i in 0..unsafe { (*t).num_iso_packets.max(0) as usize } {
                let p = unsafe { &*iso(t, i) };
                if p.status != COMPLETED || p.actual_length != p.length {
                    unsafe { (*owner).iso_tx_errors += 1 };
                }
            }
        } else {
            let expected = unsafe { (*t).length - if (*slot).kind == 1 { 8 } else { 0 } };
            if unsafe { (*t).actual_length } != expected {
                unsafe { (*owner).fatal = -EIO };
            }
        }
    }
    if unsafe { (*slot).receive }
        && !stopping
        && unsafe { (*owner).fatal == 0 }
        && (status == COMPLETED || iso_loss)
    {
        if unsafe { ((*owner).ops.submit)(t) } == 0 {
            return;
        }
        unsafe { (*owner).fatal = -EIO };
    }
    unsafe { free_slot(slot) };
}

impl Usb {
    pub fn open(receiver: Receiver, receiver_data: *mut c_void) -> io::Result<Self> {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let inner = Box::new(UnsafeCell::new(Inner {
                context: ptr::null_mut(),
                handle: ptr::null_mut(),
                receiver,
                receiver_data,
                slots: [const { Slot::empty() }; TRANSFERS],
                endpoints: Endpoints::default(),
                alt: 0,
                bus: 0,
                address: 0,
                fatal: 0,
                detached: false,
                claimed0: false,
                claimed1: false,
                stopping: false,
                stopping_sco: false,
                alt6_phase: false,
                iso_rx_errors: 0,
                iso_tx_errors: 0,
                ops: NATIVE,
            }));
            let owner = inner.get();
            if libusb_init(addr_of_mut!((*owner).context)) != 0 {
                return Err(io::Error::other("libusb initialization failed"));
            }
            let mut list = ptr::null_mut();
            let count = libusb_get_device_list((*owner).context, &mut list);
            if count < 0 {
                libusb_exit((*owner).context);
                return Err(io::Error::other("USB enumeration failed"));
            }
            let mut selected = ptr::null_mut();
            let mut matches = 0;
            for i in 0..count as usize {
                let device = *list.add(i);
                let mut d = DeviceDescriptor::default();
                if libusb_get_device_descriptor(device, &mut d) == 0
                    && d.vendor == 0x0bda
                    && d.product == 0xc123
                {
                    selected = device;
                    matches += 1;
                }
            }
            let opened = matches == 1 && libusb_open(selected, addr_of_mut!((*owner).handle)) == 0;
            if opened {
                (*owner).bus = libusb_get_bus_number(selected).into();
                (*owner).address = libusb_get_device_address(selected).into();
            }
            libusb_free_device_list(list, 1);
            if !opened {
                libusb_exit((*owner).context);
                return Err(io::Error::other(format!(
                    "need exactly one accessible Realtek 0bda:c123 controller (found {matches})"
                )));
            }
            for index in 0..TRANSFERS {
                (*owner).slots[index].owner = owner;
            }
            let mut usb = Self { inner };
            if let Err(e) = usb.load_descriptors(2) {
                drop(usb);
                return Err(io::Error::from_raw_os_error(-e));
            }
            if libusb_kernel_driver_active((*usb.inner.get()).handle, 0) == 0
                && libusb_attach_kernel_driver((*usb.inner.get()).handle, 0) != 0
            {
                return Err(io::Error::other("could not restore btusb before bootstrap"));
            }
            Ok(usb)
        }
    }
    fn load_descriptors(&mut self, wanted: i32) -> Result<(), i32> {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            (*owner).endpoints = ((*owner).ops.descriptors)((*owner).handle, wanted)?;
            Ok(())
        }
    }
    pub fn bus(&self) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();
            (*owner).bus
        }
    }
    pub fn address(&self) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();
            (*owner).address
        }
    }
    pub fn wbs_packet_size(&self) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();
            (*owner).endpoints.wbs_size
        }
    }
    fn allocate(
        &mut self,
        kind: u8,
        receive: bool,
        bytes: usize,
        packets: i32,
    ) -> Result<*mut Slot, i32> {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            let Some(index) = (*owner).slots.iter().position(|s| s.transfer.is_null()) else {
                return Err(-ENOBUFS);
            };
            let slot = addr_of_mut!((*owner).slots[index]);
            let transfer = ((*owner).ops.alloc)(packets);
            if transfer.is_null() {
                return Err(-ENOMEM);
            }
            let buffer = ((*owner).ops.alloc_buffer)(bytes);
            if buffer.is_null() {
                ((*owner).ops.free_transfer)(transfer);
                return Err(-ENOMEM);
            }
            {
                (*slot).kind = kind;
                (*slot).receive = receive;
                (*slot).transfer = transfer;
                (*transfer).buffer = buffer;
                (*transfer).user_data = slot.cast();
            }
            Ok(slot)
        }
    }
    unsafe fn submit_slot(&mut self, slot: *mut Slot) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            let rc = ((*owner).ops.submit)((*slot).transfer);
            if rc != 0 {
                free_slot(slot);
            }
            rc
        }
    }
    fn receive(&mut self, kind: u8) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            let packets = if kind == 3 { RX_ISO_PACKETS } else { 0 };
            let size = if kind == 3 {
                (*owner).endpoints.iso_size * packets
            } else if kind == 4 {
                260
            } else {
                4096
            };
            let slot = match self.allocate(kind, true, size as usize, packets) {
                Ok(slot) => slot,
                Err(error) => return error,
            };
            let t = (*slot).transfer;
            {
                (*t).dev_handle = (*owner).handle;
                (*t).endpoint = if kind == 3 {
                    (*owner).endpoints.iso_in
                } else if kind == 4 {
                    (*owner).endpoints.interrupt_in
                } else {
                    (*owner).endpoints.bulk_in
                };
                (*t).transfer_type = if kind == 3 {
                    TYPE_ISO
                } else if kind == 4 {
                    TYPE_INTERRUPT
                } else {
                    TYPE_BULK
                };
                (*t).timeout = 0;
                (*t).length = size;
                (*t).num_iso_packets = packets;
                (*t).callback = Some(completed);
                for i in 0..packets as usize {
                    (*iso(t, i)).length = (*owner).endpoints.iso_size as u32;
                }
                self.submit_slot(slot)
            }
        }
    }
    fn start_receivers(&mut self) -> i32 {
        for kind in [4, 2, 2] {
            let error = self.receive(kind);
            if error != 0 {
                return error;
            }
        }
        0
    }
    pub fn claim(&mut self) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            if libusb_detach_kernel_driver((*owner).handle, 0) != 0 {
                return -EBUSY;
            }
            (*owner).detached = true;
            if libusb_claim_interface((*owner).handle, 0) != 0 {
                return -EBUSY;
            }
            (*owner).claimed0 = true;
            if libusb_claim_interface((*owner).handle, 1) != 0 {
                return -EBUSY;
            }
            (*owner).claimed1 = true;
            if ((*owner).ops.set_alt)((*owner).handle, 0) != 0 {
                return -EIO;
            }
            self.start_receivers()
        }
    }
    pub fn pump(&mut self, ms: i32) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();
            let rc = ((*owner).ops.events)((*owner).context, ms);
            if rc != 0 && rc != LIBUSB_ERROR_INTERRUPTED {
                rc
            } else {
                (*owner).fatal
            }
        }
    }
    fn cancel(&mut self, iso_only: bool) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            let Ok(start) = ((*owner).ops.now_ms)() else {
                return -EIO;
            };
            let deadline = start + 2000;
            for index in 0..TRANSFERS {
                let slot = addr_of_mut!((*owner).slots[index]);
                let transfer = (*slot).transfer;
                if !transfer.is_null() && (!iso_only || (*slot).kind == 3) {
                    ((*owner).ops.cancel)(transfer);
                }
            }
            while (*owner).active(iso_only) != 0 {
                let Ok(now) = ((*owner).ops.now_ms)() else {
                    return -EIO;
                };
                if now >= deadline {
                    return -ETIMEDOUT;
                }
                let remaining = (deadline - now) as i32;
                self.pump(remaining.min(10));
            }
            0
        }
    }
    pub fn set_sco(&mut self, mode: i32) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            if mode != 0 && mode != 2 && mode != 3 {
                return -EINVAL;
            }
            let alt = if mode == 2 {
                2
            } else if mode == 3 {
                (*owner).endpoints.wbs_alt
            } else {
                0
            };
            if mode != 0 && alt == 0 {
                return -ENOTSUP;
            }
            if alt == (*owner).alt {
                return 0;
            }
            (*owner).stopping_sco = true;
            let rc = self.cancel(true);
            if rc != 0 {
                return rc;
            }
            if (*owner).fatal != 0 {
                return (*owner).fatal;
            }
            if ((*owner).ops.set_alt)((*owner).handle, 0) != 0 {
                return -EIO;
            }
            (*owner).alt = 0;
            if (*owner).iso_rx_errors != 0 || (*owner).iso_tx_errors != 0 {
                eprintln!(
                    "floss-usb: SCO USB packet loss: RX {}, TX {}",
                    (*owner).iso_rx_errors,
                    (*owner).iso_tx_errors
                );
            }
            (*owner).iso_rx_errors = 0;
            (*owner).iso_tx_errors = 0;
            if alt == 0 {
                (*owner).stopping_sco = false;
                return 0;
            }
            if self.load_descriptors(alt).is_err()
                || ((*owner).ops.set_alt)((*owner).handle, alt) != 0
            {
                return -EIO;
            }
            (*owner).alt = alt;
            (*owner).stopping_sco = false;
            if self.receive(3) != 0 || self.receive(3) != 0 {
                return -EIO;
            }
            eprintln!(
                "floss-usb: SCO air mode {mode}, USB alternate {alt}, endpoint bytes {}",
                (*owner).endpoints.iso_size
            );
            0
        }
    }
    pub fn send(&mut self, kind: u8, data: &[u8]) -> i32 {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            if data.is_empty() || data.len() > 4096 || !matches!(kind, 1..=3) {
                return -EINVAL;
            }
            if kind == 3 && (*owner).alt == 0 {
                return -ENOTCONN;
            }
            let alt6 = kind == 3 && (*owner).alt == 6;
            if alt6 && data.len() > (*owner).endpoints.iso_size as usize {
                return -EINVAL;
            }
            let packets = if alt6 {
                if (*owner).alt6_phase { 8 } else { 7 }
            } else if kind == 3 {
                (data.len() as i32 + (*owner).endpoints.iso_size - 1) / (*owner).endpoints.iso_size
            } else {
                0
            };
            let bytes = data.len() + if kind == 1 { 8 } else { 0 };
            let slot = match self.allocate(kind, false, bytes, packets) {
                Ok(slot) => slot,
                Err(error) => return error,
            };
            let t = (*slot).transfer;
            {
                (*t).dev_handle = (*owner).handle;
                (*t).callback = Some(completed);
                (*t).timeout = 1000;
                (*t).num_iso_packets = packets;
                if kind == 1 {
                    (*t).transfer_type = TYPE_CONTROL;
                    (*t).endpoint = 0;
                    (*t).length = bytes as i32;
                    ptr::write_bytes((*t).buffer, 0, 8);
                    *(*t).buffer = 0x20;
                    (*t).buffer.add(6).write((data.len() & 0xff) as u8);
                    (*t).buffer.add(7).write((data.len() >> 8) as u8);
                    ptr::copy_nonoverlapping(data.as_ptr(), (*t).buffer.add(8), data.len());
                } else {
                    (*t).transfer_type = if kind == 2 { TYPE_BULK } else { TYPE_ISO };
                    (*t).endpoint = if kind == 2 {
                        (*owner).endpoints.bulk_out
                    } else {
                        (*owner).endpoints.iso_out
                    };
                    (*t).length = data.len() as i32;
                    ptr::copy_nonoverlapping(data.as_ptr(), (*t).buffer, data.len());
                    if alt6 {
                        for i in 0..packets as usize - 1 {
                            (*iso(t, i)).length = 0;
                        }
                        (*iso(t, packets as usize - 1)).length = data.len() as u32;
                    } else {
                        let mut remain = data.len();
                        for i in 0..packets as usize {
                            let n = remain.min((*owner).endpoints.iso_size as usize);
                            (*iso(t, i)).length = n as u32;
                            remain -= n;
                        }
                    }
                }
                let rc = self.submit_slot(slot);
                if rc == 0 && alt6 {
                    (*owner).alt6_phase = !(*owner).alt6_phase;
                }
                rc
            }
        }
    }
}
impl Drop for Usb {
    fn drop(&mut self) {
        // SAFETY: stable callback allocation; only raw field accesses cross FFI.
        unsafe {
            let owner = self.inner.get();

            (*owner).stopping = true;
            if !(*owner).context.is_null() && self.cancel(false) != 0 {
                eprintln!("floss-usb: USB cancellation timed out");
                _exit(1)
            }
            {
                if !(*owner).handle.is_null() {
                    if (*owner).claimed1 {
                        ((*owner).ops.set_alt)((*owner).handle, 0);
                        libusb_release_interface((*owner).handle, 1);
                    }
                    if (*owner).claimed0 {
                        libusb_release_interface((*owner).handle, 0);
                    }
                    if (*owner).detached && libusb_attach_kernel_driver((*owner).handle, 0) != 0 {
                        eprintln!("floss-usb: could not reattach btusb");
                    }
                    libusb_close((*owner).handle);
                }
                if !(*owner).context.is_null() {
                    libusb_exit((*owner).context);
                }
            }
        }
    }
}

static STOPPING: AtomicBool = AtomicBool::new(false);
extern "C" fn stop_signal(_: c_int) {
    STOPPING.store(true, Ordering::Relaxed);
}
pub fn install_signals() {
    unsafe {
        signal(15, stop_signal as *const () as usize);
        signal(2, stop_signal as *const () as usize);
        signal(13, 1);
    }
}
pub fn stopping() -> bool {
    STOPPING.load(Ordering::Relaxed)
}
pub fn poll_fd(fd: i32, ms: i32) -> i32 {
    let mut p = PollFd {
        fd,
        events: 1,
        revents: 0,
    };
    let rc = unsafe { poll(&mut p, 1, ms) };
    if rc < 0 {
        let e = unsafe { *__errno_location() };
        if e == 4 { 0 } else { -e }
    } else if p.revents & (8 | 16 | 32) != 0 {
        -EIO
    } else {
        i32::from(rc > 0 && p.revents & 1 != 0)
    }
}
pub fn bootstrap(index: u32) -> i32 {
    if index > 65535 {
        return -EINVAL;
    }
    let fd = unsafe { socket(31, 3 | 0x80000, 1) };
    if fd < 0 {
        return -unsafe { *__errno_location() };
    }
    #[repr(C)]
    struct Address {
        family: u16,
        index: u16,
        channel: u16,
    }
    let address = Address {
        family: 31,
        index: index as u16,
        channel: 1,
    };
    let rc = unsafe {
        bind(
            fd,
            (&address as *const Address).cast(),
            std::mem::size_of::<Address>() as u32,
        )
    };
    let saved = unsafe { *__errno_location() };
    unsafe { close(fd) };
    if rc < 0 { -saved } else { 0 }
}

#[cfg(test)]
mod abi_tests;
#[cfg(test)]
mod tests;
