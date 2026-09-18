/* Test fixture only: private-bus HFP session/PCM peer, no Bluetooth codecs. */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static void entry(DBusMessageIter *dict, const char *key, int type, const void *value) {
    DBusMessageIter item, variant; char signature[2] = {type, 0};
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &item);
    dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&item, DBUS_TYPE_VARIANT, signature, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&item, &variant);
    dbus_message_iter_close_container(dict, &item);
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "normal";
    DBusError error = DBUS_ERROR_INIT;
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (!bus || dbus_bus_request_name(bus, "org.chromium.bluetooth", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return 2;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0), pcm = -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    int a2dp = !strncmp(mode, "a2dp", 4);
    strcpy(address.sun_path, a2dp ? "/var/run/bluetooth/audio/.a2dp_data" : "/var/run/bluetooth/audio/.sco_data");
    unlink(address.sun_path);
    if (bind(listener, (void *)&address, sizeof(address)) || listen(listener, 1)) { perror("listen"); return 2; }
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, SIG_IGN);
    uint64_t received = 0, nonzero = 0, sent = 0, ticks = 0;
    unsigned positions = 0, configurations = 0; int stopped = 0, released = 0;
    char owner[256] = "", callback[256] = "";
    struct timespec deadline; clock_gettime(CLOCK_MONOTONIC, &deadline);
    setbuf(stdout, NULL); puts("ready");
    while (running) {
        dbus_connection_read_write(bus, 0);
        DBusMessage *m;
        while ((m = dbus_connection_pop_message(bus))) {
            if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) { dbus_message_unref(m); continue; }
            const char *method = dbus_message_get_member(m);
            DBusMessage *reply = dbus_message_new_method_return(m);
            dbus_bool_t yes = TRUE;
            if (!strcmp(method, "RegisterCallback")) {
                const char *path;
                assert(dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                snprintf(owner, sizeof(owner), "%s", dbus_message_get_sender(m));
                snprintf(callback, sizeof(callback), "%s", path);
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "IsInitialized")) {
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "ReserveAudioSession")) {
                uint64_t token = 42;
                const char *device, *path; dbus_bool_t hfp;
                assert(dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &device, DBUS_TYPE_BOOLEAN, &hfp, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                assert(!strcmp(device, "AA:BB:CC:DD:EE:FF") && hfp == !a2dp);
                assert(!strcmp(path, callback) && !strcmp(dbus_message_get_sender(m), owner));
                dbus_message_append_args(reply, DBUS_TYPE_UINT64, &token, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "StartAudioSession")) {
                uint64_t token; const char *path; int fd;
                if (!dbus_message_get_args(m, NULL, DBUS_TYPE_UINT64, &token, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) || token != 42) abort();
                assert(!strcmp(path, callback) && !strcmp(dbus_message_get_sender(m), owner));
                char status = 1; if (write(fd, &status, 1) != 1) abort(); close(fd);
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "GetAudioSessionPosition")) {
                DBusMessageIter root, dict; struct timespec stamp; clock_gettime(CLOCK_MONOTONIC_RAW, &stamp);
                int64_t sec = stamp.tv_sec, nsec = stamp.tv_nsec; uint64_t delay = 0, bytes = received;
                if (!strcmp(mode, "a2dp-counter-reset") && ++positions > 5) bytes = 0;
                dbus_message_iter_init_append(reply, &root);
                dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
                entry(&dict, "valid", DBUS_TYPE_BOOLEAN, &yes);
                entry(&dict, "total_bytes_read", DBUS_TYPE_UINT64, &bytes);
                entry(&dict, "data_position_sec", DBUS_TYPE_INT64, &sec);
                entry(&dict, "data_position_nsec", DBUS_TYPE_INT64, &nsec);
                entry(&dict, "remote_delay_report_ns", DBUS_TYPE_UINT64, &delay);
                dbus_message_iter_close_container(&root, &dict);
            } else if (!strcmp(method, "GetHfpPcmConfig") || !strcmp(method, "GetA2dpPcmConfig")) {
                DBusMessageIter root, dict; uint32_t rate = a2dp ? 48000 : 16000, codec = 2;
                unsigned char bits = 16, channels = a2dp ? 2 : 1; uint64_t generation = 1;
                const char *path = address.sun_path;
                configurations++;
                if (!strcmp(mode, "bad-format")) rate = 48000;
                if (!strcmp(mode, "generation-change") && configurations > 1) generation = 2;
                dbus_message_iter_init_append(reply, &root);
                dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
                entry(&dict, "ready", DBUS_TYPE_BOOLEAN, &yes); entry(&dict, "active", DBUS_TYPE_BOOLEAN, &yes);
                entry(&dict, "sample_rate", DBUS_TYPE_UINT32, &rate); entry(&dict, "codec", DBUS_TYPE_UINT32, &codec);
                entry(&dict, "bits_per_sample", DBUS_TYPE_BYTE, &bits); entry(&dict, "channels_count", DBUS_TYPE_BYTE, &channels);
                entry(&dict, "generation", DBUS_TYPE_UINT64, &generation); entry(&dict, "socket_path", DBUS_TYPE_STRING, &path);
                dbus_message_iter_close_container(&root, &dict);
            } else if (!strcmp(method, "StopAudioSession")) {
                uint64_t token; const char *path;
                assert(dbus_message_get_args(m, NULL, DBUS_TYPE_UINT64, &token, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                assert(token == 42 && !strcmp(path, callback) && !strcmp(dbus_message_get_sender(m), owner));
                stopped++; puts("lease-stopped");
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else {
                fprintf(stderr, "unexpected method: %s\n", method); abort();
            }
            dbus_connection_send(bus, reply, NULL); dbus_connection_flush(bus);
            dbus_message_unref(reply); dbus_message_unref(m);
        }
        if (pcm < 0) pcm = accept4(listener, NULL, NULL, SOCK_NONBLOCK);
        if (pcm >= 0) {
            if (!strcmp(mode, "owner-loss") && !released && ++ticks == 30) {
                dbus_bus_release_name(bus, "org.chromium.bluetooth", NULL); released = 1;
            }
            unsigned char data[8192]; ssize_t n;
            while ((n = recv(pcm, data, a2dp ? 1920 : sizeof(data), MSG_DONTWAIT)) > 0) {
                received += n; for (int i = 0; i < n; i++) if (data[i]) nonzero++;
                if (a2dp) break;
            }
            if (!strcmp(mode, "disconnect") && ++ticks == 30) { close(pcm); pcm = -1; }
            else if (!a2dp) {
                int16_t tone[160]; for (unsigned i = 0; i < 160; i++) tone[i] = (i % 32 < 16) ? 12000 : -12000;
                n = send(pcm, tone, sizeof(tone), MSG_NOSIGNAL | MSG_DONTWAIT); if (n > 0) sent += n;
            }
        }
        deadline.tv_nsec += 10000000;
        if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    printf("received=%llu nonzero=%llu sent=%llu stopped=%d\n", (unsigned long long)received, (unsigned long long)nonzero, (unsigned long long)sent, stopped);
    if (pcm >= 0) close(pcm);
    close(listener);
    dbus_connection_close(bus); dbus_connection_unref(bus);
    return 0;
}
