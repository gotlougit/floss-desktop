/* SPDX-License-Identifier: MIT
 * Small libusb boundary. Protocol parsing and connection state live in Rust.
 * All callbacks run on the caller's event thread; no background USB threads.
 */
#include "usb.h"
#include <errno.h>
#include <libusb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TRANSFERS 72
#define RX_ISO_PACKETS 10
struct slot {
  struct usb_transport *owner;
  struct libusb_transfer *transfer;
  unsigned char kind;
  bool receive;
};
struct usb_transport {
  libusb_context *context;
  libusb_device_handle *handle;
  usb_receiver receiver;
  void *receiver_data;
  struct slot slots[TRANSFERS];
  unsigned char interrupt_in, bulk_in, bulk_out, iso_in, iso_out;
  int iso_size, alt, wbs_alt, wbs_size;
  int bus, address, fatal;
  bool detached, claimed0, claimed1, stopping, stopping_sco;
};
static volatile sig_atomic_t stopping;
static void stop_signal(int signal) {
  (void)signal;
  stopping = 1;
}
void transport_install_signals(void) {
  struct sigaction action = {.sa_handler = stop_signal};
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  signal(SIGPIPE, SIG_IGN);
}
int transport_stopping(void) { return stopping; }
int transport_poll_fd(int fd, int ms) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  int r = poll(&p, 1, ms);
  if (r < 0 && errno == EINTR)
    return 0;
  if (r < 0)
    return -errno;
  if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
    return -EIO;
  return r > 0 && (p.revents & POLLIN);
}
static int active(struct usb_transport *u, bool iso_only) {
  int n = 0;
  for (int i = 0; i < TRANSFERS; i++)
    if (u->slots[i].transfer && (!iso_only || u->slots[i].kind == 3))
      n++;
  return n;
}
static void free_slot(struct slot *s) {
  free(s->transfer->buffer);
  libusb_free_transfer(s->transfer);
  s->transfer = NULL;
}
static void completed(struct libusb_transfer *t) {
  struct slot *s = t->user_data;
  struct usb_transport *u = s->owner;
  bool stopping_here = u->stopping || (s->kind == 3 && u->stopping_sco);
  if (t->status == LIBUSB_TRANSFER_CANCELLED && !stopping_here)
    u->fatal = -ECANCELED;
  if (t->status != LIBUSB_TRANSFER_COMPLETED &&
      t->status != LIBUSB_TRANSFER_CANCELLED)
    u->fatal = -EIO;
  if (t->status == LIBUSB_TRANSFER_COMPLETED && !stopping_here) {
    if (s->receive) {
      if (s->kind == 3) {
        for (int i = 0; i < t->num_iso_packets; i++) {
          struct libusb_iso_packet_descriptor *p = &t->iso_packet_desc[i];
          if (p->status != LIBUSB_TRANSFER_COMPLETED) {
            u->fatal = -EIO;
            break;
          }
          if (p->actual_length)
            u->receiver(u->receiver_data, s->kind,
                        libusb_get_iso_packet_buffer(t, i),
                        (int)p->actual_length);
        }
      } else if (t->actual_length) {
        u->receiver(u->receiver_data, s->kind, t->buffer, t->actual_length);
      }
    } else {
      if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        for (int i = 0; i < t->num_iso_packets; i++)
          if (t->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED ||
              t->iso_packet_desc[i].actual_length !=
                  t->iso_packet_desc[i].length)
            u->fatal = -EIO;
      } else if (t->actual_length !=
                 t->length -
                     (s->kind == 1 ? (int)LIBUSB_CONTROL_SETUP_SIZE : 0)) {
        u->fatal = -EIO;
      }
    }
  }
  if (s->receive && !stopping_here && !u->fatal &&
      t->status == LIBUSB_TRANSFER_COMPLETED) {
    if (libusb_submit_transfer(t) == 0)
      return;
    u->fatal = -EIO;
  }
  free_slot(s);
}
static struct slot *allocate(struct usb_transport *u, int kind, bool receive,
                             int bytes, int packets) {
  for (int i = 0; i < TRANSFERS; i++) {
    struct slot *s = &u->slots[i];
    if (s->transfer)
      continue;
    s->owner = u;
    s->kind = (unsigned char)kind;
    s->receive = receive;
    s->transfer = libusb_alloc_transfer(packets);
    if (!s->transfer)
      return NULL;
    s->transfer->buffer = calloc(1, (size_t)bytes);
    if (!s->transfer->buffer) {
      libusb_free_transfer(s->transfer);
      s->transfer = NULL;
      return NULL;
    }
    return s;
  }
  return NULL;
}
static int submit(struct slot *s) {
  if (!s)
    return -ENOBUFS;
  int rc = libusb_submit_transfer(s->transfer);
  if (rc)
    free_slot(s);
  return rc;
}
static int receive(struct usb_transport *u, int kind) {
  int packets = kind == 3 ? RX_ISO_PACKETS : 0;
  int size = kind == 3 ? u->iso_size * packets : kind == 4 ? 260 : 4096;
  struct slot *s = allocate(u, kind, true, size, packets);
  if (!s)
    return -ENOMEM;
  struct libusb_transfer *t = s->transfer;
  unsigned char *buf = t->buffer;
  if (kind == 3) {
    libusb_fill_iso_transfer(t, u->handle, u->iso_in, buf, size, packets,
                             completed, s, 0);
    libusb_set_iso_packet_lengths(t, u->iso_size);
  } else if (kind == 4)
    libusb_fill_interrupt_transfer(t, u->handle, u->interrupt_in, buf, size,
                                   completed, s, 0);
  else
    libusb_fill_bulk_transfer(t, u->handle, u->bulk_in, buf, size, completed, s,
                              0);
  return submit(s);
}
int usb_transport_pump(struct usb_transport *u, int ms) {
  struct timeval timeout = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  int rc = libusb_handle_events_timeout_completed(u->context, &timeout, NULL);
  if (rc && rc != LIBUSB_ERROR_INTERRUPTED)
    return rc;
  return u->fatal;
}
static int cancel(struct usb_transport *u, bool iso_only) {
  for (int i = 0; i < TRANSFERS; i++) {
    struct slot *s = &u->slots[i];
    if (s->transfer && (!iso_only || s->kind == 3))
      libusb_cancel_transfer(s->transfer);
  }
  for (int n = 0; active(u, iso_only) && n < 200; n++)
    usb_transport_pump(u, 10);
  return active(u, iso_only) ? -ETIMEDOUT : 0;
}
static int descriptors(struct usb_transport *u, int wanted_alt) {
  struct libusb_config_descriptor *config = NULL;
  if (libusb_get_active_config_descriptor(libusb_get_device(u->handle),
                                          &config))
    return -EIO;
  bool found = false;
  for (int i = 0; i < config->bNumInterfaces; i++) {
    const struct libusb_interface *interface = &config->interface[i];
    for (int a = 0; a < interface->num_altsetting; a++) {
      const struct libusb_interface_descriptor *alt = &interface->altsetting[a];
      if (alt->bInterfaceClass != 0xe0 || alt->bInterfaceSubClass != 1 ||
          alt->bInterfaceProtocol != 1)
        continue;
      unsigned char in = 0, out = 0;
      int in_size = 0, out_size = 0;
      for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
        int type = ep->bmAttributes & 3;
        if (alt->bInterfaceNumber == 0 && alt->bAlternateSetting == 0) {
          if (type == 3 && (ep->bEndpointAddress & 0x80))
            u->interrupt_in = ep->bEndpointAddress;
          if (type == 2 && (ep->bEndpointAddress & 0x80))
            u->bulk_in = ep->bEndpointAddress;
          if (type == 2 && !(ep->bEndpointAddress & 0x80))
            u->bulk_out = ep->bEndpointAddress;
        }
        if (alt->bInterfaceNumber == 1 && type == 1) {
          int size = ep->wMaxPacketSize & 0x7ff;
          if (ep->bEndpointAddress & 0x80) {
            in = ep->bEndpointAddress;
            in_size = size;
          } else {
            out = ep->bEndpointAddress;
            out_size = size;
          }
        }
      }
      if (alt->bInterfaceNumber == 1 && in && out && in_size == out_size &&
          in_size > 0) {
        if (alt->bAlternateSetting == 1 && !u->wbs_alt) {
          u->wbs_alt = 1;
          u->wbs_size = 24;
        }
        if (alt->bAlternateSetting == 6 && in_size >= 63) {
          u->wbs_alt = 6;
          u->wbs_size = 60;
        }
        if (alt->bAlternateSetting == wanted_alt) {
          u->iso_in = in;
          u->iso_out = out;
          u->iso_size = in_size;
          found = true;
        }
      }
    }
  }
  libusb_free_config_descriptor(config);
  return found && u->interrupt_in && u->bulk_in && u->bulk_out ? 0 : -ENOTSUP;
}
struct usb_transport *usb_transport_open(usb_receiver callback, void *opaque) {
  struct usb_transport *u = calloc(1, sizeof(*u));
  if (!u)
    return NULL;
  if (libusb_init(&u->context)) {
    free(u);
    return NULL;
  }
  libusb_device **devices = NULL;
  ssize_t count = libusb_get_device_list(u->context, &devices);
  if (count < 0) {
    usb_transport_close(u);
    return NULL;
  }
  libusb_device *selected = NULL;
  int matches = 0;
  for (ssize_t i = 0; i < count; i++) {
    struct libusb_device_descriptor d;
    if (!libusb_get_device_descriptor(devices[i], &d) && d.idVendor == 0x0bda &&
        d.idProduct == 0xc123) {
      selected = devices[i];
      matches++;
    }
  }
  if (matches != 1 || libusb_open(selected, &u->handle)) {
    fprintf(stderr,
            "floss-usb: need exactly one accessible Realtek 0bda:c123 "
            "controller (found %d)\n",
            matches);
    libusb_free_device_list(devices, 1);
    usb_transport_close(u);
    return NULL;
  }
  u->bus = libusb_get_bus_number(selected);
  u->address = libusb_get_device_address(selected);
  libusb_free_device_list(devices, 1);
  u->receiver = callback;
  u->receiver_data = opaque;
  if (descriptors(u, 2)) {
    usb_transport_close(u);
    return NULL;
  }
  /* Recover after an unclean helper exit. The normal kernel driver performs
   * vendor firmware setup before userspace takes over this initialized radio.
   */
  if (libusb_kernel_driver_active(u->handle, 0) == 0 &&
      libusb_attach_kernel_driver(u->handle, 0)) {
    usb_transport_close(u);
    return NULL;
  }
  return u;
}
int usb_transport_bus(struct usb_transport *u) { return u->bus; }
int usb_transport_address(struct usb_transport *u) { return u->address; }
int usb_transport_wbs_packet_size(struct usb_transport *u) {
  return u->wbs_size;
}
int usb_transport_bootstrap(unsigned int index) {
  if (index > 65535)
    return -EINVAL;
  int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, 1);
  if (fd < 0)
    return -errno;
  struct {
    sa_family_t family;
    uint16_t index, channel;
  } addr = {AF_BLUETOOTH, (uint16_t)index, 1};
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  int saved = errno;
  close(fd);
  return rc < 0 ? -saved : 0;
}
int usb_transport_claim(struct usb_transport *u) {
  if (libusb_detach_kernel_driver(u->handle, 0))
    return -EBUSY;
  u->detached = true;
  if (libusb_claim_interface(u->handle, 0))
    return -EBUSY;
  u->claimed0 = true;
  if (libusb_claim_interface(u->handle, 1))
    return -EBUSY;
  u->claimed1 = true;
  if (libusb_set_interface_alt_setting(u->handle, 1, 0))
    return -EIO;
  if (receive(u, 4) || receive(u, 2))
    return -EIO;
  return 0;
}
int usb_transport_set_sco(struct usb_transport *u, int mode) {
  if (mode != 0 && mode != 2 && mode != 3)
    return -EINVAL;
  int alt = mode == 2 ? 2 : mode == 3 ? u->wbs_alt : 0;
  if (mode && !alt)
    return -ENOTSUP;
  if (alt == u->alt)
    return 0;
  u->stopping_sco = true;
  if (cancel(u, true))
    return -ETIMEDOUT;
  if (libusb_set_interface_alt_setting(u->handle, 1, 0))
    return -EIO;
  u->alt = 0;
  if (!alt) {
    u->stopping_sco = false;
    return 0;
  }
  if (descriptors(u, alt) ||
      libusb_set_interface_alt_setting(u->handle, 1, alt))
    return -EIO;
  u->alt = alt;
  u->stopping_sco = false;
  if (receive(u, 3) || receive(u, 3))
    return -EIO;
  fprintf(stderr,
          "floss-usb: SCO air mode %d, USB alternate %d, endpoint bytes %d\n",
          mode, alt, u->iso_size);
  return 0;
}
int usb_transport_send(struct usb_transport *u, unsigned char kind,
                       const unsigned char *data, int length) {
  if (length < 1 || length > 4096 || (kind != 1 && kind != 2 && kind != 3))
    return -EINVAL;
  if (kind == 3 && !u->alt)
    return -ENOTCONN;
  int packets = kind == 3 ? (length + u->iso_size - 1) / u->iso_size : 0;
  int bytes = length + (kind == 1 ? LIBUSB_CONTROL_SETUP_SIZE : 0);
  struct slot *s = allocate(u, kind, false, bytes, packets);
  if (!s)
    return -ENOBUFS;
  struct libusb_transfer *t = s->transfer;
  unsigned char *buf = t->buffer;
  if (kind == 1) {
    libusb_fill_control_setup(buf, 0x20, 0, 0, 0, (uint16_t)length);
    memcpy(buf + LIBUSB_CONTROL_SETUP_SIZE, data, length);
    libusb_fill_control_transfer(t, u->handle, buf, completed, s, 1000);
  } else {
    memcpy(buf, data, length);
    if (kind == 2)
      libusb_fill_bulk_transfer(t, u->handle, u->bulk_out, buf, length,
                                completed, s, 1000);
    else {
      libusb_fill_iso_transfer(t, u->handle, u->iso_out, buf, length, packets,
                               completed, s, 1000);
      int remain = length;
      for (int i = 0; i < packets; i++) {
        int n = remain > u->iso_size ? u->iso_size : remain;
        t->iso_packet_desc[i].length = n;
        remain -= n;
      }
    }
  }
  return submit(s);
}
void usb_transport_close(struct usb_transport *u) {
  if (!u)
    return;
  u->stopping = true;
  /* Never free live transfer callbacks. Process exit will close usbfs fds if
   * the host cannot acknowledge cancellation within the bounded deadline. */
  if (u->context && cancel(u, false)) {
    fprintf(stderr, "floss-usb: USB cancellation timed out\n");
    _exit(1);
  }
  if (u->handle) {
    if (u->claimed1) {
      libusb_set_interface_alt_setting(u->handle, 1, 0);
      libusb_release_interface(u->handle, 1);
    }
    if (u->claimed0)
      libusb_release_interface(u->handle, 0);
    if (u->detached && libusb_attach_kernel_driver(u->handle, 0))
      fprintf(stderr, "floss-usb: could not reattach btusb\n");
    libusb_close(u->handle);
  }
  if (u->context)
    libusb_exit(u->context);
  free(u);
}
