/* Test-only automatic bridge peer. PCM/configuration are synthetic; no codecs. */
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
#include <assert.h>
#include <poll.h>

static volatile sig_atomic_t running = 1;
static void quit(int sig) { (void)sig; running = 0; }
static const char *device = "AA:BB:CC:DD:EE:FF";
static const char *paths[] = {"/var/run/bluetooth/audio/.a2dp_data", "/var/run/bluetooth/audio/.sco_data"};
static void field(DBusMessageIter *dict, const char *key, int type, const void *value) {
    DBusMessageIter item, variant; char sig[] = {type, 0};
    assert(dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &item));
    assert(dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &key));
    assert(dbus_message_iter_open_container(&item, DBUS_TYPE_VARIANT, sig, &variant));
    assert(dbus_message_iter_append_basic(&variant, type, value));
    assert(dbus_message_iter_close_container(&item, &variant));
    assert(dbus_message_iter_close_container(dict, &item));
}
static void capabilities(DBusMessageIter *dict, int aac, int mono) {
    DBusMessageIter item, variant, array;
    const char *key = "a2dp_caps";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &item);
    dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&item, DBUS_TYPE_VARIANT, "aa{sv}", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "a{sv}", &array);
    for (int codec = 0; codec <= aac; codec++) {
        DBusMessageIter entry; int32_t priority = 0, rate = 3, bits = 1, channels = mono ? 1 : 2; int64_t zero = 0;
        dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY, "{sv}", &entry);
        field(&entry, "codec_type", DBUS_TYPE_INT32, &codec);
        field(&entry, "codec_priority", DBUS_TYPE_INT32, &priority);
        field(&entry, "sample_rate", DBUS_TYPE_INT32, &rate);
        field(&entry, "bits_per_sample", DBUS_TYPE_INT32, &bits);
        field(&entry, "channel_mode", DBUS_TYPE_INT32, &channels);
        for (int j = 1; j <= 4; j++) { char name[32]; snprintf(name, sizeof(name), "codec_specific_%d", j); field(&entry, name, DBUS_TYPE_INT64, &zero); }
        dbus_message_iter_close_container(&array, &entry);
    }
    dbus_message_iter_close_container(&variant, &array);
    dbus_message_iter_close_container(&item, &variant);
    dbus_message_iter_close_container(dict, &item);
}
static int listener(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path); unlink(path);
    assert(fd >= 0 && bind(fd, (void *)&a, sizeof(a)) == 0 && listen(fd, 4) == 0);
    return fd;
}
static void removed(DBusConnection *bus, const char *owner, const char *path) {
    if (!owner[0] || !path[0]) return;
    DBusMessage *m = dbus_message_new_method_call(owner, path, "org.chromium.bluetooth.BluetoothMediaCallback", "OnBluetoothAudioDeviceRemoved");
    dbus_message_set_no_reply(m, TRUE);
    dbus_message_append_args(m, DBUS_TYPE_STRING, &device, DBUS_TYPE_INVALID);
    dbus_connection_send(bus, m, NULL); dbus_connection_flush(bus); dbus_message_unref(m);
}
int main(int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "msbc";
    int aac = !strcmp(kind, "aac"), mic = strcmp(kind, "speaker") != 0 && strcmp(kind, "mono") != 0;
    uint32_t hfp_rate = !strcmp(kind, "cvsd") ? 8000 : 16000;
    uint32_t a2dp_rate = aac ? 44100 : 48000;
    unsigned char a2dp_channels = !strcmp(kind, "mono") ? 1 : 2;
    int stall_ticks = 0;
    int present = 1, pcm = -1, hfp = 0, active = 0, release_ticks = 0;
    uint64_t token = 0, next_token = 100, received = 0, total[2] = {0}, nonzero[2] = {0};
    char owner[256] = "", callback[256] = "", command[64] = "", last_command[64] = "";
    DBusError error = DBUS_ERROR_INIT;
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    assert(bus && dbus_bus_request_name(bus, "org.chromium.bluetooth", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    int sockets[] = {listener(paths[0]), listener(paths[1])};
    signal(SIGTERM, quit); signal(SIGINT, quit); signal(SIGPIPE, SIG_IGN);
    FILE *mono_samples = !strcmp(kind, "mono") ? fopen("/tmp/mock-mono.raw", "wb") : NULL;
    setbuf(stdout, NULL); printf("ready kind=%s advertised_aac=%d hfp_rate=%u\n", kind, aac, hfp_rate);
    struct timespec deadline; clock_gettime(CLOCK_MONOTONIC, &deadline);
    while (running) {
        FILE *control = fopen("/tmp/mock-control", "r");
        if (control) { if (fscanf(control, "%63s", command) != 1) command[0] = 0; fclose(control); }
        if (strcmp(command, last_command)) {
            snprintf(last_command, sizeof(last_command), "%s", command);
            printf("control=%s\n", command);
            if (!strcmp(command, "offline")) { present = 0; removed(bus, owner, callback); if (pcm >= 0) { close(pcm); pcm = -1; } }
            else if (!strcmp(command, "stall")) stall_ticks = 35;
            else if (!strcmp(command, "online")) present = 1;
            else if (!strcmp(command, "second")) present = 2;
            else if (!strcmp(command, "speaker")) mic = 0;
            else if (!strcmp(command, "headset")) mic = 1;
        }
        dbus_connection_read_write(bus, 0);
        DBusMessage *m;
        while ((m = dbus_connection_pop_message(bus))) {
            if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) { dbus_message_unref(m); continue; }
            const char *method = dbus_message_get_member(m);
            DBusMessage *reply = dbus_message_new_method_return(m);
            dbus_bool_t yes = TRUE;
            if (!strcmp(method, "RegisterCallback")) {
                const char *path; assert(dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                snprintf(owner, sizeof(owner), "%s", dbus_message_get_sender(m)); snprintf(callback, sizeof(callback), "%s", path);
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "IsInitialized")) {
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "GetConnectedAudioDevices")) {
                DBusMessageIter root, array, dict; const char *name = "Scripted headset"; int32_t cap = mic ? (hfp_rate == 8000 ? 1 : 3) : 0;
                dbus_message_iter_init_append(reply, &root); dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "a{sv}", &array);
                for (int index = 0; index < present; index++) {
                    const char *listed_device = present == 2 && index == 0 ? "11:22:33:44:55:66" : device;
                    dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY, "{sv}", &dict);
                    field(&dict, "address", DBUS_TYPE_STRING, &listed_device); field(&dict, "name", DBUS_TYPE_STRING, &name);
                    capabilities(&dict, !strcmp(kind, "hfp-only") ? -1 : aac, a2dp_channels == 1); field(&dict, "hfp_cap", DBUS_TYPE_INT32, &cap); field(&dict, "absolute_volume", DBUS_TYPE_BOOLEAN, &yes);
                    dbus_message_iter_close_container(&array, &dict);
                }
                dbus_message_iter_close_container(&root, &array);
            } else if (!strcmp(method, "ReserveAudioSession")) {
                const char *addr, *path; dbus_bool_t requested;
                assert(dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &addr, DBUS_TYPE_BOOLEAN, &requested, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                assert(!strcmp(addr, device) && !strcmp(path, callback) && !strcmp(dbus_message_get_sender(m), owner));
                assert(token == 0); hfp = requested; token = present && release_ticks == 0 && (!hfp || (mic && strcmp(kind, "denied"))) ? ++next_token : 0; received = 0;
                printf("reserve profile=%s token=%llu\n", hfp ? "hfp" : "a2dp", (unsigned long long)token);
                dbus_message_append_args(reply, DBUS_TYPE_UINT64, &token, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "StartAudioSession")) {
                uint64_t requested; const char *path; int fd;
                assert(dbus_message_get_args(m, NULL, DBUS_TYPE_UINT64, &requested, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID));
                assert(requested == token && token && !strcmp(path, callback));
                char ready = 1; assert(write(fd, &ready, 1) == 1); close(fd); active = 1;
                printf("start profile=%s token=%llu\n", hfp ? "hfp" : "a2dp", (unsigned long long)token);
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else if (!strcmp(method, "GetHfpPcmConfig") || !strcmp(method, "GetA2dpPcmConfig")) {
                if (!strcmp(method, "GetHfpPcmConfig")) {
                    const char *addr;
                    assert(dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &addr, DBUS_TYPE_INVALID));
                    assert(!strcmp(addr, device));
                } else {
                    uint64_t requested; const char *path;
                    assert(dbus_message_get_args(m, NULL, DBUS_TYPE_UINT64, &requested, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                    assert(requested == token && token && !strcmp(path, callback));
                    assert(!strcmp(dbus_message_get_sender(m), owner));
                }
                DBusMessageIter root, dict; uint32_t rate = hfp ? hfp_rate : a2dp_rate, codec = hfp_rate == 8000 ? 1 : 2;
                unsigned char bits = 16, channels = hfp ? 1 : a2dp_channels; uint64_t generation = token; const char *path = paths[hfp];
                dbus_bool_t ready = token != 0;
                dbus_message_iter_init_append(reply, &root); dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
                field(&dict, "ready", DBUS_TYPE_BOOLEAN, &ready); field(&dict, "active", DBUS_TYPE_BOOLEAN, &ready);
                field(&dict, "sample_rate", DBUS_TYPE_UINT32, &rate); field(&dict, "codec", DBUS_TYPE_UINT32, &codec);
                field(&dict, "bits_per_sample", DBUS_TYPE_BYTE, &bits); field(&dict, "channels_count", DBUS_TYPE_BYTE, &channels);
                field(&dict, "generation", DBUS_TYPE_UINT64, &generation); field(&dict, "socket_path", DBUS_TYPE_STRING, &path);
                dbus_message_iter_close_container(&root, &dict);
            } else if (!strcmp(method, "GetAudioSessionPosition")) {
                DBusMessageIter root, dict; struct timespec now; clock_gettime(CLOCK_MONOTONIC_RAW, &now);
                int64_t sec = now.tv_sec, nsec = now.tv_nsec; uint64_t delay = 0;
                dbus_message_iter_init_append(reply, &root); dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
                field(&dict, "valid", DBUS_TYPE_BOOLEAN, &yes); field(&dict, "total_bytes_read", DBUS_TYPE_UINT64, &received);
                field(&dict, "data_position_sec", DBUS_TYPE_INT64, &sec); field(&dict, "data_position_nsec", DBUS_TYPE_INT64, &nsec);
                field(&dict, "remote_delay_report_ns", DBUS_TYPE_UINT64, &delay); dbus_message_iter_close_container(&root, &dict);
            } else if (!strcmp(method, "StopAudioSession")) {
                uint64_t requested; const char *path;
                assert(dbus_message_get_args(m, NULL, DBUS_TYPE_UINT64, &requested, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
                assert(token && requested == token && !strcmp(path, callback));
                if (pcm >= 0) {
                    struct pollfd peer = {.fd = pcm, .events = POLLRDHUP};
                    assert(poll(&peer, 1, 0) >= 0);
                    assert(!(peer.revents & (POLLHUP | POLLRDHUP)));
                }
                printf("stop profile=%s token=%llu received=%llu\n", hfp ? "hfp" : "a2dp", (unsigned long long)token, (unsigned long long)received);
                token = 0; active = 0; release_ticks = !strcmp(kind, "delayed") ? 20 : 0; if (pcm >= 0) { close(pcm); pcm = -1; }
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &yes, DBUS_TYPE_INVALID);
            } else { fprintf(stderr, "unexpected method: %s\n", method); abort(); }
            dbus_connection_send(bus, reply, NULL); dbus_connection_flush(bus); dbus_message_unref(reply); dbus_message_unref(m);
        }
        if (pcm < 0 && active) pcm = accept4(sockets[hfp], NULL, NULL, SOCK_NONBLOCK);
        if (pcm >= 0 && stall_ticks == 0) {
            unsigned char data[8192]; ssize_t n; size_t period = hfp ? hfp_rate / 100 * 2 : a2dp_rate / 100 * a2dp_channels * 2;
            n = recv(pcm, data, period, MSG_DONTWAIT);
            if (n > 0) { if (mono_samples && !hfp) fwrite(data, 1, n, mono_samples); received += n; total[hfp] += n; for (ssize_t i = 0; i < n; i++) if (data[i]) nonzero[hfp]++; }
            if (hfp) {
                int16_t tone[160]; unsigned count = hfp_rate / 100;
                for (unsigned i = 0; i < count; i++) tone[i] = (i % (hfp_rate / 500) < hfp_rate / 1000) ? 12000 : -12000;
                send(pcm, tone, count * 2, MSG_NOSIGNAL | MSG_DONTWAIT);
            }
        }
        if (stall_ticks > 0) stall_ticks--;
        if (release_ticks > 0) release_ticks--;
        deadline.tv_nsec += 10000000; if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    printf("summary a2dp_bytes=%llu hfp_bytes=%llu a2dp_nonzero=%llu hfp_nonzero=%llu\n", (unsigned long long)total[0], (unsigned long long)total[1], (unsigned long long)nonzero[0], (unsigned long long)nonzero[1]);
    if (mono_samples) fclose(mono_samples);
    if (pcm >= 0) close(pcm);
    close(sockets[0]); close(sockets[1]); dbus_connection_close(bus); dbus_connection_unref(bus);
}
