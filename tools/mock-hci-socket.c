/* Test-only socket boundary shim. Use only inside the no-network smoke sandbox.
 * Production AF_BLUETOOTH/HCI calls are redirected to private UNIX packet sockets.
 * This does not emulate kernel HCI ownership, USB transport, or Bluetooth radio.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static _Atomic unsigned char mocked[65536];
int socket(int domain, int type, int protocol) {
    int (*real_socket)(int,int,int) = dlsym(RTLD_NEXT, "socket");
    if (domain != AF_BLUETOOTH || protocol != 1) return real_socket(domain,type,protocol);
    if (!getenv("MOCK_HCI_DIRECTORY")) { errno = EACCES; return -1; }
    int fd = real_socket(AF_UNIX, SOCK_SEQPACKET | (type & (SOCK_NONBLOCK | SOCK_CLOEXEC)), 0);
    if (fd >= (int)sizeof(mocked)) { close(fd); errno=EMFILE; return -1; }
    if (fd >= 0) atomic_store(&mocked[fd], 1);
    return fd;
}
int bind(int fd, const struct sockaddr *address, socklen_t length) {
    int (*real_bind)(int,const struct sockaddr*,socklen_t) = dlsym(RTLD_NEXT, "bind");
    if (fd < 0 || fd >= (int)sizeof(mocked) || !atomic_load(&mocked[fd])) return real_bind(fd,address,length);
    struct hci_address { sa_family_t family; uint16_t index, channel; } hci;
    if (length < sizeof(hci)) { errno=EINVAL; return -1; }
    memcpy(&hci,address,sizeof(hci));
    if (hci.family != AF_BLUETOOTH || (hci.channel != 1 && hci.channel != 3)) { errno=EAFNOSUPPORT; return -1; }
    const char *directory = getenv("MOCK_HCI_DIRECTORY");
    if (!directory) { errno=EACCES; return -1; }
    struct sockaddr_un peer = {.sun_family=AF_UNIX};
    int size=snprintf(peer.sun_path,sizeof(peer.sun_path),"%s/%s",directory,hci.channel==3?"mgmt":"controller");
    if (size<0 || size>=(int)sizeof(peer.sun_path)) { errno=ENAMETOOLONG; return -1; }
    fprintf(stderr,"mock-hci: channel=%u index=%u\n",hci.channel,hci.index);
    return connect(fd,(const struct sockaddr*)&peer,sizeof(peer));
}
int close(int fd) {
    int (*real_close)(int) = dlsym(RTLD_NEXT, "close");
    if (fd>=0 && fd<(int)sizeof(mocked)) atomic_store(&mocked[fd],0);
    return real_close(fd);
}
