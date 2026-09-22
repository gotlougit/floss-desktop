// SPDX-License-Identifier: MIT
use super::*;
use std::{
    cell::RefCell,
    sync::{Mutex, MutexGuard, OnceLock},
};
fn serial() -> MutexGuard<'static, ()> {
    static LOCK: Mutex<()> = Mutex::new(());
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

#[derive(Default)]
struct Mock {
    submitted: Vec<usize>,
    cancelled: Vec<usize>,
    now: i64,
    event_calls: usize,
    cancel_delay: usize,
    event_step: i64,
    fail_transfer: bool,
    fail_buffer: bool,
    fail_submit: bool,
    interrupt_events: bool,
    fail_alt: bool,
    current_alt: i32,
    alt6: bool,
}
fn state() -> &'static Mutex<Mock> {
    static STATE: OnceLock<Mutex<Mock>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(Mock::default()))
}
fn reset(alt6: bool) {
    *state().lock().unwrap() = Mock {
        event_step: 1,
        alt6,
        ..Mock::default()
    };
}

unsafe fn alloc(packets: i32) -> *mut LibusbTransfer {
    if state().lock().unwrap().fail_transfer {
        return ptr::null_mut();
    }
    let bytes = std::mem::size_of::<LibusbTransfer>()
        .max(60 + packets.max(0) as usize * std::mem::size_of::<IsoPacket>());
    unsafe { calloc(1, bytes).cast() }
}
unsafe fn free_transfer(transfer: *mut LibusbTransfer) {
    unsafe { free(transfer.cast()) }
}
unsafe fn alloc_buffer(bytes: usize) -> *mut u8 {
    if state().lock().unwrap().fail_buffer {
        ptr::null_mut()
    } else {
        unsafe { calloc(1, bytes).cast() }
    }
}
unsafe fn free_buffer(buffer: *mut u8) {
    unsafe { free(buffer.cast()) }
}
unsafe fn submit(transfer: *mut LibusbTransfer) -> i32 {
    let mut s = state().lock().unwrap();
    if s.fail_submit {
        return -1;
    }
    if s.submitted.len() == TRANSFERS {
        return -11;
    }
    s.submitted.push(transfer as usize);
    0
}
unsafe fn cancel(transfer: *mut LibusbTransfer) -> i32 {
    let mut s = state().lock().unwrap();
    let p = transfer as usize;
    if s.submitted.contains(&p) {
        if !s.cancelled.contains(&p) {
            s.cancelled.push(p);
        }
        0
    } else {
        -5
    }
}
unsafe fn events(_: *mut Context, _: i32) -> i32 {
    let callbacks = {
        let mut s = state().lock().unwrap();
        s.event_calls += 1;
        s.now += s.event_step;
        if s.event_calls <= s.cancel_delay {
            Vec::new()
        } else {
            let cancelled = std::mem::take(&mut s.cancelled);
            s.submitted.retain(|p| !cancelled.contains(p));
            cancelled
        }
    };
    for address in callbacks {
        let transfer = address as *mut LibusbTransfer;
        unsafe {
            (*transfer).status = CANCELLED;
            (*transfer).callback.unwrap()(transfer)
        };
    }
    if state().lock().unwrap().interrupt_events {
        LIBUSB_ERROR_INTERRUPTED
    } else {
        0
    }
}
unsafe fn now_ms() -> Result<i64, i32> {
    Ok(state().lock().unwrap().now)
}
unsafe fn set_alt(_: *mut DeviceHandle, alt: i32) -> i32 {
    let mut s = state().lock().unwrap();
    if s.fail_alt && alt != 0 {
        -1
    } else {
        s.current_alt = alt;
        0
    }
}
fn endpoint_set(alt6: bool, wanted: i32) -> Endpoints {
    Endpoints {
        interrupt_in: 0x81,
        bulk_in: 0x82,
        bulk_out: 2,
        iso_in: 0x83,
        iso_out: 3,
        iso_size: if wanted == 6 {
            63
        } else if wanted == 2 {
            17
        } else {
            9
        },
        wbs_alt: if alt6 { 6 } else { 1 },
        wbs_size: if alt6 { 60 } else { 24 },
    }
}
unsafe fn descriptors(_: *mut DeviceHandle, wanted: i32) -> Result<Endpoints, i32> {
    Ok(endpoint_set(state().lock().unwrap().alt6, wanted))
}
const MOCK_OPS: Ops = Ops {
    alloc,
    free_transfer,
    alloc_buffer,
    free_buffer,
    submit,
    cancel,
    events,
    now_ms,
    set_alt,
    descriptors,
};

struct Capture(RefCell<Vec<(u8, Vec<u8>)>>);
impl Default for Capture {
    fn default() -> Self {
        Self(RefCell::new(Vec::new()))
    }
}
fn receive(data: *mut c_void, kind: u8, bytes: &[u8]) {
    unsafe { &*data.cast::<Capture>() }
        .0
        .borrow_mut()
        .push((kind, bytes.to_vec()));
}
fn usb(alt6: bool, capture: &Capture) -> Usb {
    reset(alt6);
    let inner = Box::new(UnsafeCell::new(Inner {
        context: ptr::null_mut(),
        handle: ptr::null_mut(),
        receiver: receive,
        receiver_data: ptr::from_ref(capture).cast_mut().cast(),
        slots: [const { Slot::empty() }; TRANSFERS],
        endpoints: endpoint_set(alt6, 2),
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
        ops: MOCK_OPS,
    }));
    let owner = inner.get();
    for index in 0..TRANSFERS {
        unsafe { (*owner).slots[index].owner = owner };
    }
    Usb { inner }
}
fn transfers(receive: bool, kind: u8) -> Vec<*mut LibusbTransfer> {
    state()
        .lock()
        .unwrap()
        .submitted
        .iter()
        .copied()
        .map(|p| p as *mut LibusbTransfer)
        .filter(|t| {
            let slot = unsafe { &*(**t).user_data.cast::<Slot>() };
            slot.receive == receive && slot.kind == kind
        })
        .collect()
}
fn complete(t: *mut LibusbTransfer, status: i32) {
    {
        let mut s = state().lock().unwrap();
        s.submitted.retain(|p| *p != t as usize);
    }
    unsafe {
        (*t).status = status;
        (*t).callback.unwrap()(t)
    };
}
fn active(usb: &Usb, iso: bool) -> usize {
    unsafe { (*usb.inner.get()).active(iso) }
}
fn fatal(usb: &Usb) -> i32 {
    unsafe { (*usb.inner.get()).fatal }
}
fn finish(mut usb: Usb) {
    unsafe { (*usb.inner.get()).stopping = true };
    assert_eq!(usb.cancel(false), 0);
    assert_eq!(active(&usb, false), 0);
}

#[test]
fn realtek_descriptor_discovery_alt_1_2_6() {
    let _serial = serial();
    let controls = [
        Endpoint {
            address: 0x81,
            attributes: 3,
            size: 16,
        },
        Endpoint {
            address: 2,
            attributes: 2,
            size: 64,
        },
        Endpoint {
            address: 0x82,
            attributes: 2,
            size: 64,
        },
    ];
    let iso1 = [
        Endpoint {
            address: 3,
            attributes: 1,
            size: 9,
        },
        Endpoint {
            address: 0x83,
            attributes: 1,
            size: 9,
        },
    ];
    let iso2 = [
        Endpoint {
            address: 3,
            attributes: 1,
            size: 17,
        },
        Endpoint {
            address: 0x83,
            attributes: 1,
            size: 17,
        },
    ];
    let iso6 = [
        Endpoint {
            address: 3,
            attributes: 1,
            size: 63,
        },
        Endpoint {
            address: 0x83,
            attributes: 1,
            size: 63,
        },
    ];
    let base = Alternate {
        number: 0,
        alternate: 0,
        class: 0xe0,
        subclass: 1,
        protocol: 1,
        endpoints: &controls,
    };
    for (wide, size, alt) in [(&iso1[..], 24, 1), (&iso6[..], 60, 6)] {
        let all = [
            base,
            Alternate {
                number: 1,
                alternate: 1,
                class: 0xe0,
                subclass: 1,
                protocol: 1,
                endpoints: &iso1,
            },
            Alternate {
                number: 1,
                alternate: 2,
                class: 0xe0,
                subclass: 1,
                protocol: 1,
                endpoints: &iso2,
            },
            Alternate {
                number: 1,
                alternate: alt,
                class: 0xe0,
                subclass: 1,
                protocol: 1,
                endpoints: wide,
            },
        ];
        let found = discover(&all, 2).unwrap();
        assert_eq!(
            (
                found.interrupt_in,
                found.bulk_in,
                found.bulk_out,
                found.iso_size,
                found.wbs_size,
                found.wbs_alt
            ),
            (0x81, 0x82, 2, 17, size, i32::from(alt))
        );
    }
}

#[test]
fn callbacks_framing_segmentation_and_rearming() {
    let _serial = serial();
    let captured = Capture::default();
    let mut u = usb(false, &captured);
    assert_eq!(u.start_receivers(), 0);
    assert_eq!(transfers(true, 4).len(), 1);
    assert_eq!(transfers(true, 2).len(), 2);
    for t in transfers(true, 2) {
        unsafe {
            ptr::copy_nonoverlapping([1u8, 0, 1, 0, 7].as_ptr(), (*t).buffer, 5);
            (*t).actual_length = 5;
        }
        complete(t, COMPLETED);
    }
    assert_eq!(*captured.0.borrow(), vec![(2, vec![1, 0, 1, 0, 7]); 2]);
    assert_eq!(transfers(true, 2).len(), 2);
    assert_eq!(u.set_sco(3), 0);
    assert_eq!(transfers(true, 3).len(), 2);
    let sco: Vec<_> = [42, 0, 24].into_iter().chain(3..27).collect();
    assert_eq!(u.send(3, &sco), 0);
    let tx = transfers(false, 3)[0];
    unsafe {
        assert_eq!(((*tx).num_iso_packets, (*tx).length), (3, 27));
        assert_eq!(std::slice::from_raw_parts((*tx).buffer, 27), sco);
        for i in 0..3 {
            assert_eq!((*iso(tx, i)).length, 9);
            (*iso(tx, i)).actual_length = 9;
        }
    }
    complete(tx, COMPLETED);
    assert_eq!(fatal(&u), 0);
    let rx = transfers(true, 3)[0];
    unsafe {
        ptr::copy_nonoverlapping(sco.as_ptr(), (*rx).buffer, 27);
        for i in 0..RX_ISO_PACKETS as usize {
            (*iso(rx, i)).actual_length = if i < 3 { 9 } else { 0 };
        }
    }
    complete(rx, COMPLETED);
    let received: Vec<u8> = captured
        .0
        .borrow()
        .iter()
        .filter(|(k, _)| *k == 3)
        .flat_map(|(_, b)| b.clone())
        .collect();
    assert_eq!(received, sco);
    assert_eq!(transfers(true, 3).len(), 2);
    assert_eq!(u.set_sco(2), 0);
    assert_eq!(state().lock().unwrap().current_alt, 2);
    assert_eq!(transfers(true, 3).len(), 2);
    assert_eq!(u.send(3, &sco), 0);
    let cvsd = transfers(false, 3)[0];
    unsafe {
        assert_eq!((*cvsd).num_iso_packets, 2);
        assert_eq!(((*iso(cvsd, 0)).length, (*iso(cvsd, 1)).length), (17, 10));
        (*iso(cvsd, 0)).actual_length = 17;
        (*iso(cvsd, 1)).actual_length = 10;
    }
    complete(cvsd, COMPLETED);
    let command = [3, 12, 0];
    assert_eq!(u.send(1, &command), 0);
    let control = transfers(false, 1)[0];
    unsafe {
        assert_eq!((*control).length, 11);
        assert_eq!(
            std::slice::from_raw_parts((*control).buffer, 8),
            [0x20, 0, 0, 0, 0, 0, 3, 0]
        );
        assert_eq!(
            std::slice::from_raw_parts((*control).buffer.add(8), 3),
            command
        );
        (*control).actual_length = 3;
    }
    complete(control, COMPLETED);
    assert_eq!(fatal(&u), 0);
    finish(u);
}

#[test]
fn allocation_pool_submit_short_transfer_and_alt6_phase() {
    let _serial = serial();
    let captured = Capture::default();
    let mut u = usb(true, &captured);
    let acl = [1, 0, 1, 0, 7];
    state().lock().unwrap().fail_transfer = true;
    assert_eq!(u.send(2, &acl), -ENOMEM);
    assert_eq!(active(&u, false), 0);
    state().lock().unwrap().fail_transfer = false;
    state().lock().unwrap().fail_buffer = true;
    assert_eq!(u.send(2, &acl), -ENOMEM);
    assert_eq!(active(&u, false), 0);
    state().lock().unwrap().fail_buffer = false;
    assert_eq!(u.set_sco(3), 0);
    let msbc: [u8; 63] = std::array::from_fn(|index| index as u8);
    state().lock().unwrap().fail_submit = true;
    assert_ne!(u.send(3, &msbc), 0);
    assert!(!unsafe { (*u.inner.get()).alt6_phase });
    assert!(transfers(false, 3).is_empty());
    state().lock().unwrap().fail_submit = false;
    assert_eq!(u.send(3, &msbc), 0);
    let first = transfers(false, 3)[0];
    unsafe {
        assert_eq!((*first).num_iso_packets, 7);
        for i in 0..6 {
            assert_eq!((*iso(first, i)).length, 0);
        }
        assert_eq!((*iso(first, 6)).length, 63);
        assert_eq!(
            (0..6)
                .map(|i| (*iso(first, i)).length as usize)
                .sum::<usize>(),
            0
        );
        assert_eq!(std::slice::from_raw_parts((*first).buffer, 63), msbc);
        for i in 0..7 {
            (*iso(first, i)).actual_length = (*iso(first, i)).length;
        }
    }
    complete(first, COMPLETED);
    assert_eq!(u.send(3, &msbc), 0);
    let second = transfers(false, 3)[0];
    unsafe {
        assert_eq!(std::slice::from_raw_parts((*second).buffer, 63), msbc);
        assert_eq!((*second).num_iso_packets, 8);
        for i in 0..7 {
            assert_eq!((*iso(second, i)).length, 0);
        }
        assert_eq!((*iso(second, 7)).length, 63);
        for i in 0..8 {
            (*iso(second, i)).actual_length = (*iso(second, i)).length;
        }
    }
    complete(second, COMPLETED);
    assert_eq!(u.set_sco(0), 0);
    assert_eq!(u.send(3, &msbc), -ENOTCONN);
    assert_eq!(u.set_sco(9), -EINVAL);
    state().lock().unwrap().fail_alt = true;
    assert_eq!(u.set_sco(2), -EIO);
    assert_eq!(state().lock().unwrap().current_alt, 0);
    assert_eq!(active(&u, true), 0);
    state().lock().unwrap().fail_alt = false;
    for _ in 0..TRANSFERS {
        assert_eq!(u.send(2, &acl), 0);
    }
    assert_eq!(u.send(2, &acl), -ENOBUFS);
    unsafe { (*u.inner.get()).stopping = true };
    assert_eq!(u.cancel(false), 0);
    let mut u = usb(false, &captured);
    assert_eq!(u.send(2, &acl), 0);
    let short = transfers(false, 2)[0];
    unsafe {
        (*short).actual_length = 2;
    }
    complete(short, COMPLETED);
    assert_eq!(fatal(&u), -EIO);
    finish(u);
}

#[test]
fn receiver_panic_marks_transport_fatal_and_releases_completed_transfer() {
    let _serial = serial();
    let captured = Capture::default();
    let mut u = usb(false, &captured);
    fn panicking_receiver(_: *mut c_void, _: u8, _: &[u8]) {
        panic!("injected receiver failure");
    }
    unsafe { (*u.inner.get()).receiver = panicking_receiver };
    assert_eq!(u.receive(2), 0);
    let transfer = transfers(true, 2)[0];
    unsafe { (*transfer).actual_length = 1 };
    complete(transfer, COMPLETED);
    assert_eq!(fatal(&u), -EIO);
    assert_eq!(active(&u, false), 0);
    assert!(transfers(true, 2).is_empty());
    finish(u);
}

#[test]
fn cancellation_deadline_late_callbacks_and_iso_loss_policy() {
    let _serial = serial();
    for interrupted in [false, true] {
        let captured = Capture::default();
        let mut u = usb(false, &captured);
        assert_eq!(u.set_sco(3), 0);
        {
            let mut s = state().lock().unwrap();
            s.cancel_delay = 300;
            s.interrupt_events = interrupted;
        }
        let start = state().lock().unwrap().now;
        assert_eq!(u.set_sco(0), 0);
        let s = state().lock().unwrap();
        assert_eq!((s.event_calls, s.now - start), (301, 301));
        drop(s);
        assert_eq!(active(&u, true), 0);
        finish(u);
    }
    let captured = Capture::default();
    let mut u = usb(false, &captured);
    assert_eq!(u.set_sco(3), 0);
    {
        let mut s = state().lock().unwrap();
        s.cancel_delay = 10_000;
        s.event_step = 10;
    }
    let start = state().lock().unwrap().now;
    assert_eq!(u.set_sco(0), -ETIMEDOUT);
    assert_eq!(state().lock().unwrap().now - start, 2000);
    assert_eq!(active(&u, true), 2);
    {
        let mut s = state().lock().unwrap();
        s.cancel_delay = 0;
        s.event_step = 1;
        s.event_calls = 0;
    }
    assert_eq!(u.set_sco(0), 0);
    assert_eq!(active(&u, true), 0);
    assert_eq!(u.set_sco(3), 0);
    unsafe { (*u.inner.get()).fatal = -EIO };
    assert_eq!(u.set_sco(2), -EIO);
    assert_eq!(active(&u, true), 0);
    assert_eq!(state().lock().unwrap().current_alt, 1);
    unsafe { (*u.inner.get()).fatal = 0 };
    finish(u);
    let mut u = usb(false, &captured);
    assert_eq!(u.set_sco(3), 0);
    let rx = transfers(true, 3)[0];
    unsafe {
        ptr::write_bytes((*rx).buffer, 0x55, 27);
        (*iso(rx, 0)).actual_length = 9;
        (*iso(rx, 1)).status = ERROR;
        (*iso(rx, 2)).actual_length = 9;
    }
    complete(rx, COMPLETED);
    assert_eq!(
        (fatal(&u), unsafe { (*u.inner.get()).iso_rx_errors }),
        (0, 1)
    );
    assert_eq!(
        captured
            .0
            .borrow()
            .iter()
            .filter(|(k, _)| *k == 3)
            .map(|(_, b)| b.len())
            .sum::<usize>(),
        18
    );
    let sco = [0u8; 27];
    assert_eq!(u.send(3, &sco), 0);
    let tx = transfers(false, 3)[0];
    unsafe {
        for i in 0..3 {
            (*iso(tx, i)).actual_length = (*iso(tx, i)).length;
        }
        (*iso(tx, 1)).status = ERROR;
    }
    complete(tx, COMPLETED);
    assert_eq!(
        (fatal(&u), unsafe { (*u.inner.get()).iso_tx_errors }),
        (0, 1)
    );
    complete(transfers(true, 3)[0], ERROR);
    complete(transfers(true, 3)[0], TIMED_OUT);
    assert_eq!(
        (fatal(&u), unsafe { (*u.inner.get()).iso_rx_errors }),
        (0, 3)
    );
    complete(transfers(true, 3)[0], 5);
    assert_eq!(fatal(&u), -EIO);
    finish(u);
}
