// SPDX-License-Identifier: MIT
use super::*;
use std::mem::{align_of, offset_of, size_of};

// The Nix package supports x86_64 Linux. These layouts were independently
// measured against the installed public libusb 1.0.30 C header. In particular,
// the flexible ISO array starts BEFORE the end of the padded transfer struct.
#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux"))]
fn public_libusb_abi_matches_x86_64_headers() {
    macro_rules! layout {
        ($ty:ty, $size:expr, $align:expr; $($field:ident = $offset:expr),+ $(,)?) => {{
            assert_eq!(size_of::<$ty>(), $size, "{} size", stringify!($ty));
            assert_eq!(align_of::<$ty>(), $align, "{} alignment", stringify!($ty));
            $(assert_eq!(offset_of!($ty, $field), $offset,
                "{}::{} offset", stringify!($ty), stringify!($field));)+
        }};
    }
    layout!(DeviceDescriptor, 18, 2;
        length=0, descriptor_type=1, usb=2, class=4, subclass=5,
        protocol=6, max_packet=7, vendor=8, product=10, device=12,
        manufacturer=14, product_string=15, serial=16, configurations=17);
    layout!(EndpointDescriptor, 32, 8;
        length=0, descriptor_type=1, address=2, attributes=3,
        max_packet_size=4, interval=6, refresh=7, sync_address=8,
        extra=16, extra_length=24);
    layout!(InterfaceDescriptor, 40, 8;
        length=0, descriptor_type=1, number=2, alternate=3,
        endpoint_count=4, class=5, subclass=6, protocol=7,
        interface_string=8, endpoints=16, extra=24, extra_length=32);
    layout!(Interface, 16, 8; alternates=0, alternate_count=8);
    layout!(ConfigDescriptor, 40, 8;
        length=0, descriptor_type=1, total_length=2, interface_count=4,
        configuration=5, configuration_string=6, attributes=7, max_power=8,
        interfaces=16, extra=24, extra_length=32);
    layout!(LibusbTransfer, 64, 8;
        dev_handle=0, flags=8, endpoint=9, transfer_type=10, timeout=12,
        status=16, length=20, actual_length=24, callback=32,
        user_data=40, buffer=48, num_iso_packets=56, iso_packet_desc=60);
    layout!(IsoPacket, 12, 4; length=0, actual_length=4, status=8);
}

#[test]
fn descriptor_reader_handles_empty_and_malformed_arrays() {
    // All-zero descriptor structs contain only integers/raw pointers, so they
    // are valid Rust values even though they describe no usable USB endpoints.
    let mut config: ConfigDescriptor = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe { parse_descriptors(ptr::null(), 2) }.err(),
        Some(-EIO)
    );
    assert_eq!(
        unsafe { parse_descriptors(&config, 2) }.err(),
        Some(-ENOTSUP)
    );
    config.interface_count = 1;
    assert_eq!(unsafe { parse_descriptors(&config, 2) }.err(), Some(-EIO));
    let mut interface = Interface {
        alternates: ptr::null(),
        alternate_count: 0,
    };
    config.interfaces = &interface;
    assert_eq!(
        unsafe { parse_descriptors(&config, 2) }.err(),
        Some(-ENOTSUP)
    );
    interface.alternate_count = -1;
    config.interfaces = &interface;
    assert_eq!(unsafe { parse_descriptors(&config, 2) }.err(), Some(-EIO));
    interface.alternate_count = 1;
    config.interfaces = &interface;
    assert_eq!(unsafe { parse_descriptors(&config, 2) }.err(), Some(-EIO));
    let mut alt: InterfaceDescriptor = unsafe { std::mem::zeroed() };
    interface.alternates = &alt;
    config.interfaces = &interface;
    assert_eq!(
        unsafe { parse_descriptors(&config, 2) }.err(),
        Some(-ENOTSUP)
    );
    alt.endpoint_count = 1;
    interface.alternates = &alt;
    config.interfaces = &interface;
    assert_eq!(unsafe { parse_descriptors(&config, 2) }.err(), Some(-EIO));
}

#[test]
fn real_libusb_transfer_allocation_accepts_rust_iso_layout() {
    struct Allocation(*mut LibusbTransfer);
    impl Drop for Allocation {
        fn drop(&mut self) {
            unsafe { native_free_transfer(self.0) };
        }
    }
    // libusb's allocator needs no context/device. Exercise the actual library
    // allocation as well as the mocked transfer lifecycle tests.
    for packets in [0, 1, 3, 7, 8, 10] {
        let allocation = Allocation(unsafe { native_alloc(packets) });
        assert!(!allocation.0.is_null());
        unsafe {
            (*allocation.0).num_iso_packets = packets;
            for index in 0..packets as usize {
                let packet = iso(allocation.0, index);
                (*packet).length = 63;
                (*packet).actual_length = 0;
                (*packet).status = COMPLETED;
            }
        }
    }
}
