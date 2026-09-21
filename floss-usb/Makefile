# SPDX-License-Identifier: MIT
CC ?= cc
RUSTC ?= rustc
PKG_CONFIG ?= pkg-config
USB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
USB_LIBS := $(shell $(PKG_CONFIG) --libs-only-L libusb-1.0)
all: floss-usb
usb.o: src/usb.c src/usb.h
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror $(CPPFLAGS) $(CFLAGS) $(USB_CFLAGS) -c $< -o $@
libfloss_usb.a: usb.o
	$(AR) rcs $@ $<
floss-usb: src/main.rs src/transport.rs libfloss_usb.a
	$(RUSTC) --edition=2024 -D warnings -C opt-level=2 -C debuginfo=1 src/main.rs -L native=. -l static=floss_usb $(USB_LIBS) -l usb-1.0 -o $@
check: libfloss_usb.a
	$(RUSTC) --edition=2024 -D warnings --test src/main.rs -L native=. -l static=floss_usb $(USB_LIBS) -l usb-1.0 -o transport-tests
	./transport-tests
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $(CFLAGS) $(USB_CFLAGS) tests/usb-test.c $(shell $(PKG_CONFIG) --libs libusb-1.0) -o usb-tests
	./usb-tests
install: floss-usb
	install -Dm755 floss-usb $(DESTDIR)$(PREFIX)/bin/floss-usb
.PHONY: all check install
