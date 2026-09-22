# SPDX-License-Identifier: MIT
RUSTC ?= rustc
PKG_CONFIG ?= pkg-config
USB_LIBS := $(shell $(PKG_CONFIG) --libs-only-L libusb-1.0)
all: floss-usb
floss-usb: src/main.rs src/transport.rs src/usb.rs
	$(RUSTC) --edition=2024 -D warnings -C opt-level=2 -C debuginfo=1 src/main.rs $(USB_LIBS) -l usb-1.0 -o $@
check: src/main.rs src/transport.rs src/usb.rs src/usb/tests.rs src/usb/abi_tests.rs
	$(RUSTC) --edition=2024 -D warnings --test src/main.rs $(USB_LIBS) -l usb-1.0 -o transport-tests
	./transport-tests
install: floss-usb
	install -Dm755 floss-usb $(DESTDIR)$(PREFIX)/bin/floss-usb
.PHONY: all check install
