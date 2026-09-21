/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
struct usb_transport;
typedef void (*usb_receiver)(void *, unsigned char, const unsigned char *, int);
struct usb_transport *usb_transport_open(usb_receiver, void *);
int usb_transport_bus(struct usb_transport *);
int usb_transport_address(struct usb_transport *);
int usb_transport_bootstrap(unsigned int);
int usb_transport_claim(struct usb_transport *);
int usb_transport_wbs_packet_size(struct usb_transport *);
int usb_transport_set_sco(struct usb_transport *, int);
int usb_transport_send(struct usb_transport *, unsigned char,
                       const unsigned char *, int);
int usb_transport_pump(struct usb_transport *, int);
void usb_transport_close(struct usb_transport *);
int transport_poll_fd(int, int);
void transport_install_signals(void);
int transport_stopping(void);
