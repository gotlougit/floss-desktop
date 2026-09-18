/* Fixture self-check, not a test of Floss or a real Bluetooth controller. */
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int connect_channel(uint16_t index, uint16_t channel) {
    struct { sa_family_t family; uint16_t index, channel; } address = {AF_BLUETOOTH, index, channel};
    int fd = socket(AF_BLUETOOTH, SOCK_RAW, 1);
    assert(fd >= 0);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}
static void exchange(int fd, const uint8_t *command, size_t length, const uint8_t *expected, size_t size) {
    uint8_t buffer[256];
    assert(write(fd, command, length) == (ssize_t)length);
    assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)size);
    assert(memcmp(buffer, expected, size) == 0);
}
int main(void) {
    int fd = connect_channel(0xffff, 3);
    const uint8_t index_list[] = {3, 0, 255, 255, 0, 0};
    const uint8_t index_reply[] = {1, 0, 255, 255, 7, 0, 3, 0, 0, 1, 0, 0, 0};
    exchange(fd, index_list, sizeof(index_list), index_reply, sizeof(index_reply));
    close(fd);
    fd = connect_channel(0, 1);
    const uint8_t reset[] = {1, 3, 12, 0}, reset_reply[] = {4, 14, 4, 1, 3, 12, 0};
    exchange(fd, reset, sizeof(reset), reset_reply, sizeof(reset_reply));
    const uint8_t unknown[] = {1, 255, 255, 0}, unknown_reply[] = {4, 14, 4, 1, 255, 255, 1};
    exchange(fd, unknown, sizeof(unknown), unknown_reply, sizeof(unknown_reply));
    close(fd);
    puts("fixture management index, HCI Reset and Unknown Command passed");
    return 0;
}
