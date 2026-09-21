/* SPDX-License-Identifier: MIT
 * Exercise the actual libusb boundary with descriptor/transfer fixtures.
 * No USB context, device open, detach, ioctl or privileged fd is used.
 */
#include <assert.h>
#include <libusb.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static int fake_submit(struct libusb_transfer *);
static int fake_cancel(struct libusb_transfer *);
static int fake_events(libusb_context *, struct timeval *, int *);
static libusb_device *fake_device(libusb_device_handle *);
static int fake_config(libusb_device *, struct libusb_config_descriptor **);
static void fake_free_config(struct libusb_config_descriptor *);
static int fake_alt(libusb_device_handle *, int, int);
#define libusb_submit_transfer fake_submit
#define libusb_cancel_transfer fake_cancel
#define libusb_handle_events_timeout_completed fake_events
#define libusb_get_device fake_device
#define libusb_get_active_config_descriptor fake_config
#define libusb_free_config_descriptor fake_free_config
#define libusb_set_interface_alt_setting fake_alt
#include "../src/usb.c"
static struct libusb_transfer *submitted[TRANSFERS];
static bool cancelled[TRANSFERS];
static int current_alt;
static bool fail_alt;
static unsigned char captured[1024];
static size_t captured_size;
static const struct libusb_endpoint_descriptor ctrl_endpoints[] = {
    {.bEndpointAddress = 0x81, .bmAttributes = 3, .wMaxPacketSize = 16},
    {.bEndpointAddress = 0x02, .bmAttributes = 2, .wMaxPacketSize = 64},
    {.bEndpointAddress = 0x82, .bmAttributes = 2, .wMaxPacketSize = 64},
};
static struct libusb_endpoint_descriptor iso_endpoints[7][2];
static struct libusb_interface_descriptor controls = {.bInterfaceNumber = 0,
                                                      .bNumEndpoints = 3,
                                                      .bInterfaceClass = 0xe0,
                                                      .bInterfaceSubClass = 1,
                                                      .bInterfaceProtocol = 1,
                                                      .endpoint =
                                                          ctrl_endpoints};
static struct libusb_interface_descriptor alternates[7];
static struct libusb_interface interfaces[2];
static struct libusb_config_descriptor config = {.bNumInterfaces = 2,
                                                 .interface = interfaces};
static int fake_submit(struct libusb_transfer *t) {
  for (int i = 0; i < TRANSFERS; i++)
    if (!submitted[i]) {
      submitted[i] = t;
      cancelled[i] = false;
      return 0;
    }
  return LIBUSB_ERROR_NO_MEM;
}
static int fake_cancel(struct libusb_transfer *t) {
  for (int i = 0; i < TRANSFERS; i++)
    if (submitted[i] == t) {
      cancelled[i] = true;
      return 0;
    }
  return LIBUSB_ERROR_NOT_FOUND;
}
static int fake_events(libusb_context *c, struct timeval *tv, int *done) {
  (void)c;
  (void)tv;
  (void)done;
  for (int i = 0; i < TRANSFERS; i++)
    if (submitted[i] && cancelled[i]) {
      struct libusb_transfer *t = submitted[i];
      submitted[i] = NULL;
      cancelled[i] = false;
      t->status = LIBUSB_TRANSFER_CANCELLED;
      t->callback(t);
    }
  return 0;
}
static libusb_device *fake_device(libusb_device_handle *h) {
  (void)h;
  return (libusb_device *)1;
}
static int fake_config(libusb_device *d, struct libusb_config_descriptor **p) {
  (void)d;
  *p = &config;
  return 0;
}
static void fake_free_config(struct libusb_config_descriptor *c) { (void)c; }
static int fake_alt(libusb_device_handle *h, int interface, int alt) {
  (void)h;
  assert(interface == 1);
  for (int i = 0; i < TRANSFERS; i++)
    assert(!submitted[i] ||
           submitted[i]->type != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS);
  if (fail_alt && alt)
    return LIBUSB_ERROR_IO;
  current_alt = alt;
  return 0;
}
static void receiver(void *opaque, unsigned char kind,
                     const unsigned char *data, int size) {
  (void)opaque;
  assert(kind == 3);
  assert(size >= 0 && captured_size + (size_t)size <= sizeof(captured));
  memcpy(captured + captured_size, data, (size_t)size);
  captured_size += (size_t)size;
}
static void init(struct usb_transport *u, bool alt6) {
  memset(u, 0, sizeof(*u));
  u->handle = (libusb_device_handle *)1;
  u->receiver = receiver;
  assert(active(u, false) == 0);
  captured_size = 0;
  current_alt = 0;
  fail_alt = false;
  interfaces[0] =
      (struct libusb_interface){.num_altsetting = 1, .altsetting = &controls};
  interfaces[1] = (struct libusb_interface){.num_altsetting = alt6 ? 7 : 6,
                                            .altsetting = alternates};
  const int sizes[] = {0, 9, 17, 25, 33, 49, 63};
  for (int i = 0; i < 7; i++) {
    iso_endpoints[i][0] = (struct libusb_endpoint_descriptor){
        .bEndpointAddress = 3, .bmAttributes = 1, .wMaxPacketSize = sizes[i]};
    iso_endpoints[i][1] =
        (struct libusb_endpoint_descriptor){.bEndpointAddress = 0x83,
                                            .bmAttributes = 1,
                                            .wMaxPacketSize = sizes[i]};
    alternates[i] =
        (struct libusb_interface_descriptor){.bInterfaceNumber = 1,
                                             .bAlternateSetting = i,
                                             .bNumEndpoints = 2,
                                             .bInterfaceClass = 0xe0,
                                             .bInterfaceSubClass = 1,
                                             .bInterfaceProtocol = 1,
                                             .endpoint = iso_endpoints[i]};
  }
  assert(descriptors(u, 2) == 0);
}
static struct libusb_transfer *find(bool rx, int kind) {
  for (int i = 0; i < TRANSFERS; i++)
    if (submitted[i]) {
      struct slot *s = submitted[i]->user_data;
      if (s->receive == rx && s->kind == kind)
        return submitted[i];
    }
  assert(false);
  return NULL;
}
static void complete(struct libusb_transfer *t) {
  for (int i = 0; i < TRANSFERS; i++)
    if (submitted[i] == t) {
      submitted[i] = NULL;
      t->status = LIBUSB_TRANSFER_COMPLETED;
      t->callback(t);
      return;
    }
  assert(false);
}
int main(void) {
  struct usb_transport u;
  init(&u, false);
  assert(u.wbs_alt == 1 && u.wbs_size == 24);
  assert(usb_transport_set_sco(&u, 3) == 0 && current_alt == 1 &&
         active(&u, true) == 2);
  unsigned char sco[27] = {42, 0, 24};
  for (int i = 3; i < 27; i++)
    sco[i] = (unsigned char)i;
  assert(usb_transport_send(&u, 3, sco, sizeof(sco)) == 0);
  struct libusb_transfer *tx = find(false, 3);
  assert(tx->num_iso_packets == 3 && tx->length == 27 &&
         !memcmp(tx->buffer, sco, 27));
  for (int i = 0; i < 3; i++) {
    assert(tx->iso_packet_desc[i].length == 9);
    tx->iso_packet_desc[i].actual_length = 9;
  }
  complete(tx);
  assert(!u.fatal);
  struct libusb_transfer *rx = find(true, 3);
  memcpy(rx->buffer, sco, 27);
  for (int i = 0; i < RX_ISO_PACKETS; i++)
    rx->iso_packet_desc[i].actual_length = i < 3 ? 9 : 0;
  complete(rx);
  assert(captured_size == 27 && !memcmp(captured, sco, 27));
  assert(active(&u, true) == 2); // Receive transfer was rearmed.
  assert(usb_transport_set_sco(&u, 2) == 0 && current_alt == 2 &&
         active(&u, true) == 2);
  const unsigned char command[] = {3, 12, 0};
  assert(usb_transport_send(&u, 1, command, sizeof(command)) == 0);
  tx = find(false, 1);
  assert(tx->length == 11 && tx->buffer[0] == 0x20 &&
         !memcmp(tx->buffer + 8, command, 3));
  tx->actual_length = 3;
  complete(tx);
  assert(!u.fatal);
  assert(usb_transport_set_sco(&u, 0) == 0 && active(&u, true) == 0 &&
         current_alt == 0);
  assert(usb_transport_send(&u, 3, sco, sizeof(sco)) == -ENOTCONN);
  assert(usb_transport_set_sco(&u, 9) == -EINVAL);
  init(&u, true);
  assert(u.wbs_size == 60 && u.wbs_alt == 6);
  assert(usb_transport_set_sco(&u, 3) == 0 && current_alt == 6);
  assert(usb_transport_set_sco(&u, 0) == 0);
  fail_alt = true;
  assert(usb_transport_set_sco(&u, 2) < 0 && current_alt == 0 &&
         active(&u, true) == 0);
  fail_alt = false;
  const unsigned char acl[] = {1, 0, 1, 0, 7};
  for (int i = 0; i < TRANSFERS; i++)
    assert(usb_transport_send(&u, 2, acl, sizeof(acl)) == 0);
  assert(usb_transport_send(&u, 2, acl, sizeof(acl)) == -ENOBUFS);
  u.stopping = true;
  assert(cancel(&u, false) == 0 && !u.fatal);
  init(&u, false);
  assert(usb_transport_send(&u, 2, acl, sizeof(acl)) == 0);
  tx = find(false, 2);
  tx->actual_length = 2;
  complete(tx);
  assert(u.fatal == -EIO);
  puts("PASS: actual USB callbacks, Realtek descriptors, mSBC24/60, CVSD, ISO "
       "RX/TX splitting, control transfers, cancellation, bounds and "
       "short-transfer failure");
}
