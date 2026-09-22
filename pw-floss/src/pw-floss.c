/* PipeWire */
/* SPDX-License-Identifier: MIT */

/* Experimental single-headset Floss bridge. See PROTOCOL.md before use.
 * Floss owns the Bluetooth codecs; this program transfers interleaved PCM.
 * Deliberately runs processing on the main loop, not the realtime thread.
 */
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <pipewire/pipewire.h>
#include <spa/monitor/device.h>
#include <spa/param/profile.h>
#include <spa/pod/filter.h>

#define SERVICE "org.chromium.bluetooth"
#define MEDIA_IFACE SERVICE ".BluetoothMedia"
#define CALLBACK_IFACE SERVICE ".BluetoothMediaCallback"
#define CALLBACK_PATH "/org/pipewire/FlossAudio/callback"
/* Global across adapters/users: Floss currently has a single A2DP PCM socket. */
#define LOCK_NAME "org.pipewire.FlossAudio"
#define AUDIO_PATH "/var/run/bluetooth/audio/.a2dp_data"
#define SCO_PATH "/var/run/bluetooth/audio/.sco_data"
#define CALL_TIMEOUT_MS 3000
#define START_TIMEOUT_MS 10000
#define A2DP_CODEC_COUNT 5
#define QUEUE_SIZE (96000 * 8 / 10) /* At most 100 ms of stereo S32 at 96 kHz. */
#define TARGET_DELAY_SECONDS 0.030
#define MAX_RATE_CORRECTION 0.01
#define POSITION_TIMEOUT_MS 2000

static volatile sig_atomic_t startup_cancelled;

static void cancel_startup(int signal_number)
{
	(void)signal_number;
	startup_cancelled = 1;
}

/* Queue error is measured in seconds. A bounded PI loop controls the adaptive
 * resampler; no Bluetooth codec processing happens here. */
struct clock_controller {
    double integral, filtered_error;
    uint64_t update_ns;
    bool ready;
};

struct bridge {
    struct pw_context *context;
    struct pw_core *core;
    uint32_t card_id;
    struct spa_hook playback_listener, capture_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct spa_list graph_objects;
    bool capture_demand;
    struct spa_device card;
    struct spa_hook_list card_listeners;
    struct spa_param_info card_params[2];
    struct pw_properties *card_properties;
    struct pw_proxy *card_proxy;
    struct spa_hook card_proxy_listener;
    int requested_profile, selected_profile;
    bool profile_save, repair_layout;
    int32_t codec_rates[A2DP_CODEC_COUNT], codec_modes[A2DP_CODEC_COUNT], codec_bits[A2DP_CODEC_COUNT];
	struct pw_main_loop *main;
	struct pw_stream *stream;
	struct pw_stream *capture;
	struct spa_source *audio_source;
	struct spa_source *bus_timer;
	DBusConnection *bus;
	char *owner;
	char path[96];
	char address[18];
	char device_name[249];
	int audio_fd;
	bool failed;
	bool starting;
	dbus_uint64_t session;
	bool hfp;
	bool automatic, microphone, transitioning, a2dp_available;
    bool hfp_transport_unavailable;
    uint32_t available_profiles;
    unsigned capture_overruns;
    bool playback_format_ready, capture_format_ready;
    int64_t discovery_deadline, hfp_retry_deadline;
	unsigned adapter;
	uint32_t capture_rate;
	int64_t capture_idle_ms;
	bool capture_running;
	bool playback_running;
	uint32_t rate;
	uint32_t channels;
    uint8_t bits;
	size_t queue_limit;
	uint8_t queue[QUEUE_SIZE];
	size_t read_pos, queued;
	uint8_t capture_queue[QUEUE_SIZE];
	size_t capture_read_pos, capture_queued;
	uint8_t voice_queue[QUEUE_SIZE];
	size_t voice_read_pos, voice_queued;
	size_t voice_credit;
	uint64_t generation;
    struct clock_controller playback_clock, capture_clock;
    bool voice_primed, capture_primed;
    uint64_t audio_written;
    DBusPendingCall *position_call;
    uint64_t position_bytes, position_ns, position_received_ns, remote_delay_ns;
    bool position_valid;
    unsigned position_ticks;
    uint64_t playback_latency_ns, capture_latency_ns;
    uint64_t playback_latency_update_ns, capture_latency_update_ns;
    uint64_t position_progress_ns, position_request_ns;
};

#include "profiles.h"
#include "capture-demand.h"

struct hfp_pcm_config {
	dbus_bool_t ready;
	dbus_bool_t active;
	dbus_uint32_t rate, codec;
	uint8_t bits, channels;
	dbus_uint64_t generation;
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static void fail(struct bridge *b, const char *reason)
{
	if (!b->failed)
		fprintf(stderr, "pw-floss: %s\n", reason);
	b->failed = true;
	if (b->main && !b->transitioning)
		pw_main_loop_quit(b->main);
}

static DBusMessage *call(struct bridge *b, const char *method,
		int first_type, ...)
{
	DBusMessage *m, *reply;
	DBusError error = DBUS_ERROR_INIT;
	va_list args;

	/* Pin calls to the original unique owner: never affect a replacement daemon. */
	m = dbus_message_new_method_call(b->owner, b->path, MEDIA_IFACE, method);
	if (!m)
		return NULL;
	va_start(args, first_type);
	if (!dbus_message_append_args_valist(m, first_type, args)) {
		va_end(args);
		dbus_message_unref(m);
		return NULL;
	}
	va_end(args);
	reply = dbus_connection_send_with_reply_and_block(b->bus, m,
			CALL_TIMEOUT_MS, &error);
	dbus_message_unref(m);
	if (!reply) {
		fprintf(stderr, "pw-floss: %s: %s\n", method,
			error.message ? error.message : "no reply");
		dbus_error_free(&error);
	}
	return reply;
}

static bool bool_reply(DBusMessage *reply, bool *value)
{
	dbus_bool_t result;
	bool valid = reply && dbus_message_has_signature(reply, "b") &&
		dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &result,
			DBUS_TYPE_INVALID);
	if (valid)
		*value = result;
	if (reply)
		dbus_message_unref(reply);
	return valid;
}

static DBusHandlerResult callback(DBusConnection *conn, DBusMessage *m, void *data)
{
	struct bridge *b = data;
	DBusMessage *reply;
	const char *sender = dbus_message_get_sender(m);
	const char *address;

	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!sender || !b->owner || strcmp(sender, b->owner) != 0) {
		reply = dbus_message_new_error(m, DBUS_ERROR_ACCESS_DENIED,
				"Only the registered Floss daemon may call this object");
	} else if (!dbus_message_has_interface(m, CALLBACK_IFACE)) {
		reply = dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"Unknown callback interface");
	} else if (dbus_message_is_method_call(m, CALLBACK_IFACE,
			"OnBluetoothAudioDeviceRemoved") ||
			(b->hfp && dbus_message_is_method_call(m, CALLBACK_IFACE,
				"OnHfpAudioDisconnected"))) {
		if (!dbus_message_has_signature(m, "s") ||
		    !dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &address,
			    DBUS_TYPE_INVALID)) {
			reply = dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
					"Expected device address");
		} else {
			if (strcasecmp(address, b->address) == 0) {
				fail(b, "selected audio device disconnected; restart after reconnecting");
			}
			reply = dbus_message_new_method_return(m);
		}
	} else {
		/* Automatic discovery reconciles additions and capabilities from
         * the next authoritative snapshot. Volume and LE are separate APIs. */
		reply = dbus_message_new_method_return(m);
	}
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult bus_filter(DBusConnection *conn, DBusMessage *m, void *data)
{
	struct bridge *b = data;
	const char *name, *old_owner, *new_owner;
	const char *sender = dbus_message_get_sender(m);
	(void)conn;

	if (sender && strcmp(sender, DBUS_SERVICE_DBUS) == 0 &&
	    dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
	    dbus_message_has_signature(m, "sss") &&
	    dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name,
		    DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &new_owner,
		    DBUS_TYPE_INVALID) && strcmp(name, SERVICE) == 0 && b->owner &&
	    strcmp(new_owner, b->owner) != 0)
		fail(b, "Floss owner changed; restart bridge after the daemon recovers");
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* A bounded, nonblocking dispatch on the non-RT main loop. */
static void dispatch_bus(struct bridge *b)
{
	unsigned i;
	if (!dbus_connection_read_write(b->bus, 0)) {
		fail(b, "system bus disconnected");
		return;
	}
	for (i = 0; i < 32 && !b->failed; i++)
		if (dbus_connection_dispatch(b->bus) != DBUS_DISPATCH_DATA_REMAINS)
			break;
}

static void poll_position(struct bridge *b);
static void automatic_tick(struct bridge *b);

static void bus_tick(void *data, uint64_t expirations)
{
    struct bridge *b = data;
    (void)expirations;
    dispatch_bus(b);
    if (b->automatic && !b->starting && !b->failed) automatic_tick(b);
    if (!b->hfp && !b->failed && ++b->position_ticks >= 5) {
        b->position_ticks = 0;
        poll_position(b);
    }
}

static bool setup_bus(struct bridge *b)
{
	DBusError error = DBUS_ERROR_INIT;
	DBusMessage *m, *reply;
	const char *service = SERVICE, *owner;
	const char *callback_path = CALLBACK_PATH;
	bool result;
	int lock_result;
	static const DBusObjectPathVTable vtable = { .message_function = callback };

	b->bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (!b->bus)
		goto error;
	dbus_connection_set_exit_on_disconnect(b->bus, FALSE);
	if (!dbus_connection_can_send_type(b->bus, DBUS_TYPE_UNIX_FD)) {
		fail(b, "system bus does not support Unix FD passing");
		return false;
	}
	lock_result = dbus_bus_request_name(b->bus, LOCK_NAME,
			DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
	if (dbus_error_is_set(&error))
		goto error;
	if (lock_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fail(b, "another Floss audio bridge owns the global PCM endpoint");
		return false;
	}
	if (!dbus_connection_add_filter(b->bus, bus_filter, b, NULL))
		return false;
	dbus_bus_add_match(b->bus, "type='signal',sender='org.freedesktop.DBus',"
		"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
		"arg0='org.chromium.bluetooth'", &error);
	if (dbus_error_is_set(&error))
		goto error;
	m = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
			DBUS_INTERFACE_DBUS, "GetNameOwner");
	if (!m)
		return false;
	if (!dbus_message_append_args(m, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID)) {
		dbus_message_unref(m);
		return false;
	}
	reply = dbus_connection_send_with_reply_and_block(b->bus, m, CALL_TIMEOUT_MS, &error);
	dbus_message_unref(m);
	if (!reply)
		goto error;
	if (!dbus_message_has_signature(reply, "s") ||
	    !dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID)) {
		dbus_message_unref(reply);
		return false;
	}
	b->owner = strdup(owner);
	dbus_message_unref(reply);
	if (!b->owner)
		return false;
	if (!bool_reply(call(b, "IsInitialized", DBUS_TYPE_INVALID), &result))
		return false;
	if (!result) {
		fail(b, "Floss media is not initialized; wait for adapter readiness and retry");
		return false;
	}
	if (!dbus_connection_register_object_path(b->bus, CALLBACK_PATH, &vtable, b))
		return false;
	if (!bool_reply(call(b, "RegisterCallback", DBUS_TYPE_OBJECT_PATH,
			&callback_path, DBUS_TYPE_INVALID), &result) || !result)
		return false;
	return true;
error:
	fprintf(stderr, "pw-floss: system bus: %s\n",
		error.message ? error.message : "connection failed");
	dbus_error_free(&error);
	return false;
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The integration's getter reports the live native SCO feeding configuration, not
 * selectable HFP capabilities or the legacy codec notification byte. */
static bool get_pcm_config(struct bridge *b, struct hfp_pcm_config *config)
{
	const char *address = b->address;
	const char *callback_path = CALLBACK_PATH;
	DBusMessage *reply = b->hfp ? call(b, "GetHfpPcmConfig", DBUS_TYPE_STRING,
			&address, DBUS_TYPE_INVALID) : call(b, "GetA2dpPcmConfig",
			DBUS_TYPE_UINT64, &b->session, DBUS_TYPE_OBJECT_PATH, &callback_path, DBUS_TYPE_INVALID);
	DBusMessageIter outer, entries;
	unsigned seen = 0;
	bool valid = false;

	memset(config, 0, sizeof(*config));
	if (!reply || !dbus_message_has_signature(reply, "a{sv}"))
		goto done;
	dbus_message_iter_init(reply, &outer);
	dbus_message_iter_recurse(&outer, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;
		void *target = NULL;
		int type = DBUS_TYPE_INVALID;
		unsigned bit = 0;

		dbus_message_iter_recurse(&entries, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);
		if (strcmp(key, "ready") == 0) {
			type = DBUS_TYPE_BOOLEAN; target = &config->ready; bit = 1;
		} else if (strcmp(key, "active") == 0) {
			type = DBUS_TYPE_BOOLEAN; target = &config->active; bit = 128;
		} else if (strcmp(key, "sample_rate") == 0) {
			type = DBUS_TYPE_UINT32; target = &config->rate; bit = 2;
		} else if (strcmp(key, "bits_per_sample") == 0) {
			type = DBUS_TYPE_BYTE; target = &config->bits; bit = 4;
		} else if (strcmp(key, "channels_count") == 0) {
			type = DBUS_TYPE_BYTE; target = &config->channels; bit = 8;
		} else if (strcmp(key, "codec") == 0) {
			type = DBUS_TYPE_UINT32; target = &config->codec; bit = 16;
		} else if (strcmp(key, "generation") == 0) {
			type = DBUS_TYPE_UINT64; target = &config->generation; bit = 32;
		} else if (strcmp(key, "socket_path") == 0) {
			type = DBUS_TYPE_STRING; bit = 64;
		}
		if (bit) {
			if ((seen & bit) || dbus_message_iter_get_arg_type(&value) != type)
				goto done;
			seen |= bit;
			if (type == DBUS_TYPE_STRING) {
				const char *path;
				dbus_message_iter_get_basic(&value, &path);
				if (strlen(path) >= sizeof(config->socket_path))
					goto done;
				snprintf(config->socket_path, sizeof(config->socket_path), "%s", path);
			} else {
				dbus_message_iter_get_basic(&value, target);
			}
		}
		dbus_message_iter_next(&entries);
	}
	if (b->hfp)
        valid = (seen & 161) == 161 && (!config->ready || (seen == 255 && config->active && config->generation &&
            config->bits == 16 && config->channels == 1 &&
            ((config->codec == 1 && config->rate == 8000) || (config->codec == 2 && config->rate == 16000)) &&
            strcmp(config->socket_path, SCO_PATH) == 0));
    else
        valid = (seen & 33) == 33 && (!config->ready ||
            ((seen & 111) == 111 && config->generation && (config->bits == 16 || config->bits == 24 || config->bits == 32) && (config->channels == 1 || config->channels == 2) &&
             (config->rate == 44100 || config->rate == 48000 || config->rate == 88200 || config->rate == 96000) && !strcmp(config->socket_path, AUDIO_PATH)));

done:
	if (reply)
		dbus_message_unref(reply);
	if (!valid)
		fail(b, "missing or unsupported negotiated PCM configuration");
	return valid;
}

static uint64_t raw_time_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
}

/* Position RPCs run asynchronously on the same main loop as PCM processing.
 * The native counter includes bytes consumed from the Unix socket, so socket
 * buffering is measured as well as our own ring; POLLOUT alone is not a clock. */
static void position_received(DBusPendingCall *pending, void *data)
{
    struct bridge *b = data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    DBusMessageIter outer, entries;
    dbus_bool_t valid = FALSE;
    dbus_uint64_t bytes = 0, remote = 0;
    dbus_int64_t sec = 0, nsec = 0;
    unsigned seen = 0;
    uint64_t now = raw_time_ns(), stamp;

    b->position_call = NULL;
    dbus_pending_call_unref(pending);
    if (!reply || dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR ||
            !dbus_message_has_signature(reply, "a{sv}")) goto invalid;
    dbus_message_iter_init(reply, &outer);
    dbus_message_iter_recurse(&outer, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, value;
        const char *key;
        void *target = NULL;
        int type = DBUS_TYPE_INVALID;
        unsigned bit = 0;
        dbus_message_iter_recurse(&entries, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &value);
        if (!strcmp(key, "valid")) { target = &valid; type = DBUS_TYPE_BOOLEAN; bit = 1; }
        else if (!strcmp(key, "total_bytes_read")) { target = &bytes; type = DBUS_TYPE_UINT64; bit = 2; }
        else if (!strcmp(key, "data_position_sec")) { target = &sec; type = DBUS_TYPE_INT64; bit = 4; }
        else if (!strcmp(key, "data_position_nsec")) { target = &nsec; type = DBUS_TYPE_INT64; bit = 8; }
        else if (!strcmp(key, "remote_delay_report_ns")) { target = &remote; type = DBUS_TYPE_UINT64; bit = 16; }
        if (bit) {
            if ((seen & bit) || dbus_message_iter_get_arg_type(&value) != type) goto invalid;
            dbus_message_iter_get_basic(&value, target);
            seen |= bit;
        }
        dbus_message_iter_next(&entries);
    }
    if (seen != 31 || !valid || sec < 0 || nsec < 0 || nsec >= 1000000000 ||
            (uint64_t)sec > (UINT64_MAX - (uint64_t)nsec) / 1000000000u || bytes > b->audio_written ||
            (b->position_valid && bytes < b->position_bytes)) goto invalid;
    stamp = (uint64_t)sec * 1000000000u + nsec;
    if (stamp > now || (b->position_valid && stamp < b->position_ns)) goto invalid;
    if (!b->position_valid || bytes != b->position_bytes || !b->playback_running)
        b->position_progress_ns = now;
    b->position_bytes = bytes;
    b->position_ns = stamp;
    b->position_received_ns = now;
    b->remote_delay_ns = remote;
    b->position_valid = true;
    dbus_message_unref(reply);
    return;
invalid:
    if (reply) dbus_message_unref(reply);
    fail(b, "A2DP lease position unavailable or transport counter reset");
}

static void poll_position(struct bridge *b)
{
    const char *owner = CALLBACK_PATH;
    DBusMessage *message;
    if (b->position_call) {
        /* Keep the deadline independent of libdbus timeout-loop integration. */
        if (raw_time_ns() - b->position_request_ns > (uint64_t)POSITION_TIMEOUT_MS * 1000000u)
            fail(b, "A2DP position request timed out");
        return;
    }
    if (!b->session) return;
    b->position_request_ns = raw_time_ns();
    message = dbus_message_new_method_call(b->owner, b->path, MEDIA_IFACE, "GetAudioSessionPosition");
    if (!message) { fail(b, "could not allocate position request"); return; }
    if (!dbus_message_append_args(message, DBUS_TYPE_UINT64, &b->session,
            DBUS_TYPE_OBJECT_PATH, &owner, DBUS_TYPE_INVALID) ||
            !dbus_connection_send_with_reply(b->bus, message, &b->position_call, POSITION_TIMEOUT_MS) ||
            !b->position_call) {
        dbus_message_unref(message);
        fail(b, "could not request A2DP transport position");
        return;
    }
    dbus_message_unref(message);
    if (!dbus_pending_call_set_notify(b->position_call, position_received, b, NULL)) {
        dbus_pending_call_cancel(b->position_call);
        dbus_pending_call_unref(b->position_call);
        b->position_call = NULL;
        fail(b, "could not watch A2DP transport position");
    }
}

/* Extrapolate the native MONOTONIC_RAW sample timestamp between position
 * replies. Clamp consumption to bytes actually submitted; include unsent PCM.
 * A stale counter does not silently turn into an unbounded rate correction. */
static double playback_level(struct bridge *b, uint64_t now)
{
    double consumed, bytes_per_second = (double)b->rate * b->channels * (b->bits / 8);
    if (b->hfp) return (double)b->voice_queued / bytes_per_second;
    if (!b->position_valid) return -1.0;
    if (now - b->position_received_ns > (uint64_t)POSITION_TIMEOUT_MS * 1000000u) {
        fail(b, "A2DP position feedback stalled");
        return -1.0;
    }
    if (b->playback_running && b->audio_written > b->position_bytes &&
            now - b->position_progress_ns > (uint64_t)POSITION_TIMEOUT_MS * 1000000u) {
        fail(b, "A2DP transport stopped consuming submitted PCM");
        return -1.0;
    }
    consumed = b->position_bytes;
    if (b->position_ns && now >= b->position_ns)
        /* Do not extrapolate an old read across arbitrarily long idle gaps.
         * A fresh snapshot is normally available every 100 ms. */
        consumed += (double)SPA_MIN(now - b->position_ns, (uint64_t)200000000u) /
            1e9 * bytes_per_second;
    if (consumed > b->audio_written) consumed = b->audio_written;
    return ((double)b->queued + b->audio_written - consumed) / bytes_per_second;
}

static double clamp_double(double value, double low, double high)
{
    return value < low ? low : value > high ? high : value;
}

/* audioconvert's resample-native divides input rate by the rate multiplier.
 * Higher rate thus produces more PCM into a sink's ring, or consumes fewer PCM
 * frames from a source's ring. Both directions need rate < 1 when too full.
 * A positive rate also activates adaptive resampling even at equal nominal
 * rates (audioconvert.c: resample_is_passthrough). */
static void adapt_rate(struct bridge *b, struct pw_stream *stream,
        struct clock_controller *clock, double level, uint64_t now)
{
    double dt, error, candidate, correction;
    int result;
    if (!clock->ready) {
        clock->ready = true;
        clock->update_ns = now;
        clock->integral = clock->filtered_error = 0.0;
    }
    dt = clamp_double((double)(now - clock->update_ns) / 1e9, 0.0, 0.25);
    clock->update_ns = now;
    if (level < 0.0) {
        correction = 0.0;
    } else {
        error = TARGET_DELAY_SECONDS - level;
        /* One-second low pass suppresses graph/SCO packet-boundary jitter. */
        clock->filtered_error += dt / (1.0 + dt) * (error - clock->filtered_error);
        candidate = clamp_double(clock->integral + 0.02 * clock->filtered_error * dt,
                -MAX_RATE_CORRECTION, MAX_RATE_CORRECTION);
        correction = 0.20 * clock->filtered_error + candidate;
        /* Anti-windup: integrate only when not pushing further into a clamp. */
        if ((correction < MAX_RATE_CORRECTION || clock->filtered_error < 0.0) &&
            (correction > -MAX_RATE_CORRECTION || clock->filtered_error > 0.0))
            clock->integral = candidate;
        correction = clamp_double(0.20 * clock->filtered_error + clock->integral,
                -MAX_RATE_CORRECTION, MAX_RATE_CORRECTION);
    }
    result = pw_stream_set_rate(stream, 1.0 + correction);
    if (result < 0) {
        /* Called only from process, once graph IO exists. A bypassed adapter
         * cannot provide clock recovery; don't pretend that it can. */
        fail(b, "PipeWire adaptive resampler is unavailable for this stream");
    }
}

static void report_latency(struct bridge *b, struct pw_stream *stream, bool capture,
        uint64_t delay)
{
    uint64_t *previous = capture ? &b->capture_latency_ns : &b->playback_latency_ns;
    uint64_t *updated = capture ? &b->capture_latency_update_ns : &b->playback_latency_update_ns;
    uint64_t now = raw_time_ns();
    uint64_t difference = delay > *previous ? delay - *previous : *previous - delay;
    uint8_t storage[128];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    struct spa_process_latency_info info = SPA_PROCESS_LATENCY_INFO_INIT(.ns = (int64_t)delay);
    const struct spa_pod *param;
    if (difference < 2000000u || now - *updated < 100000000u) return;
    param = spa_process_latency_build(&builder, SPA_PARAM_ProcessLatency, &info);
    if (pw_stream_update_params(stream, &param, 1) < 0) {
        fail(b, "could not publish Bluetooth buffering latency");
        return;
    }
    *previous = delay;
    *updated = now;
}

static bool reserve_audio(struct bridge *b)
{
    const char *address = b->address, *owner = CALLBACK_PATH;
    dbus_bool_t hfp = b->hfp;
    int64_t deadline = monotonic_ms() + CALL_TIMEOUT_MS;
    do {
        DBusMessage *reply = call(b, "ReserveAudioSession", DBUS_TYPE_STRING, &address,
            DBUS_TYPE_BOOLEAN, &hfp, DBUS_TYPE_OBJECT_PATH, &owner, DBUS_TYPE_INVALID);
        bool valid = reply && dbus_message_has_signature(reply, "t") &&
            dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT64, &b->session, DBUS_TYPE_INVALID);
        if (reply) dbus_message_unref(reply);
        if (!valid) break;
        if (b->session) return true;
        /* Stop acknowledges cancellation, not its native terminal callback.
         * Floss retains the old lease until that callback; allow it to settle
         * without destroying nodes or racing a still-running SCO transport. */
        if (!b->transitioning) break;
        dispatch_bus(b);
        if (b->failed || startup_cancelled) break;
        poll(NULL, 0, 50);
    } while (monotonic_ms() < deadline);
    fail(b, "Floss refused the address-scoped audio reservation");
    return false;
}

static bool request_start(struct bridge *b, int listener)
{
    const char *owner = CALLBACK_PATH;
    bool accepted = false;
    if (!bool_reply(call(b, "StartAudioSession", DBUS_TYPE_UINT64, &b->session,
            DBUS_TYPE_OBJECT_PATH, &owner, DBUS_TYPE_UNIX_FD, &listener,
            DBUS_TYPE_INVALID), &accepted) || !accepted) {
        fail(b, "Floss did not accept the reserved audio start");
        return false;
    }
    return true;
}

static bool wait_start_listener(struct bridge *b, int listener, bool hfp)
{
	int64_t deadline = monotonic_ms() + START_TIMEOUT_MS;
	while (!b->failed && !startup_cancelled && monotonic_ms() < deadline) {
		uint8_t status;
		struct pollfd p = { .fd = listener, .events = POLLIN };
		int res = poll(&p, 1, 50);
		dispatch_bus(b);
		if (res < 0 && errno != EINTR)
			break;
		if (res > 0) {
			ssize_t count = read(listener, &status, 1);
			if (count == 1)
				return hfp ? status != 0 : status == 1;
			if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR))
				break;
		}
	}
	return false;
}

/* A connected SCO link and a listening PCM socket do not prove that the
 * controller's host transport actually delivers audio (notably USB HCI USER).
 * Require a complete incoming sample before committing the desktop profile. */
static bool hfp_pcm_arrives(struct bridge *b)
{
    int64_t deadline = monotonic_ms() + 2000;
    while (!b->failed && !startup_cancelled && monotonic_ms() < deadline) {
        uint8_t sample[2];
        ssize_t n = recv(b->audio_fd, sample, sizeof(sample), MSG_PEEK | MSG_DONTWAIT);
        if (n == sizeof(sample)) return true;
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        dispatch_bus(b);
        poll(NULL, 0, 20);
    }
    b->hfp_transport_unavailable = true;
    fail(b, "HFP connected without microphone PCM; controller SCO transport is unavailable");
    return false;
}

static bool start_hfp(struct bridge *b)
{
    struct hfp_pcm_config config, confirmed;
    int listener[2];
    bool accepted, notified;
    int64_t deadline;

    if (startup_cancelled || !reserve_audio(b)) return false;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, listener) < 0)
        return false;
    accepted = request_start(b, listener[1]);
    close(listener[1]);
    notified = accepted && wait_start_listener(b, listener[0], true);
    close(listener[0]);
    if (!notified || b->failed || startup_cancelled) {
        fail(b, "Floss did not complete HFP start successfully");
        return false;
    }
	/* The AudioConnected callback precedes native UIPC socket creation.
	 * Retry ready config plus connect, never infer PCM from the wakeup byte. */
	deadline = monotonic_ms() + START_TIMEOUT_MS;
	while (!b->failed && !startup_cancelled && monotonic_ms() < deadline) {
		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		if (!get_pcm_config(b, &config))
			return false;
		if (config.ready) {
			b->generation = config.generation;
			b->audio_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
			if (b->audio_fd < 0)
				return false;
			snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config.socket_path);
			if (connect(b->audio_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				if (!get_pcm_config(b, &confirmed) || !confirmed.ready ||
				    confirmed.generation != config.generation ||
				    confirmed.rate != config.rate || confirmed.codec != config.codec) {
					fail(b, "HFP transport changed while opening PCM");
					return false;
				}
				b->rate = confirmed.rate;
				b->channels = confirmed.channels;
				b->bits = confirmed.bits;
				b->generation = confirmed.generation;
				return hfp_pcm_arrives(b);
			}
			int error = errno;
			close(b->audio_fd);
			b->audio_fd = -1;
			if (error != ENOENT && error != ECONNREFUSED && error != EAGAIN && error != EINTR) {
				fprintf(stderr, "pw-floss: HFP PCM connect: %s\n", strerror(error));
				return false;
			}
		}
		dispatch_bus(b);
		poll(NULL, 0, 50);
	}
	fail(b, "Floss HFP PCM socket did not become ready");
	return false;
}

static bool start_audio(struct bridge *b)
{
    struct hfp_pcm_config config, confirmed;
    int listener[2];
    bool accepted, notified;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    if (startup_cancelled || !reserve_audio(b)) return false;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, listener) < 0)
        return false;
    accepted = request_start(b, listener[1]);
    close(listener[1]);
    notified = accepted && wait_start_listener(b, listener[0], false);
    close(listener[0]);
    if (!notified || b->failed || startup_cancelled) {
        fail(b, "Floss did not complete A2DP start successfully");
        return false;
    }
    if (!get_pcm_config(b, &config) || !config.ready) return false;
    /* Native admission and start bind the selected address to our token. No
     * void SetActiveDevice or global Start/Stop request is used by the bridge. */
    b->audio_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (b->audio_fd < 0) return false;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", AUDIO_PATH);
    if (connect(b->audio_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "pw-floss: connect %s: %s\n", AUDIO_PATH, strerror(errno));
        return false;
    }
    if (!get_pcm_config(b, &confirmed) || !confirmed.ready ||
            confirmed.generation != config.generation || confirmed.rate != config.rate ||
            confirmed.channels != config.channels || confirmed.bits != config.bits) {
        fail(b, "A2DP configuration changed while opening PCM");
        return false;
    }
    b->rate = confirmed.rate;
    b->channels = confirmed.channels;
    b->bits = confirmed.bits;
    b->generation = confirmed.generation;
    return true;
}

static void flush_audio(struct bridge *b)
{
	/* At most QUEUE_SIZE bytes per invocation; never block the audio loop. */
	while (b->queued && !b->failed) {
		size_t count = SPA_MIN(b->queued, QUEUE_SIZE - b->read_pos);
		ssize_t n = send(b->audio_fd, b->queue + b->read_pos, count,
				MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;
		if (n <= 0) {
			fail(b, "Floss PCM socket closed or write failed");
			return;
		}
		b->read_pos = (b->read_pos + n) % QUEUE_SIZE;
		b->queued -= n;
        b->audio_written += n;
	}
	if (b->audio_source)
		pw_loop_update_io(pw_main_loop_get_loop(b->main), b->audio_source,
				SPA_IO_ERR | SPA_IO_HUP | (b->hfp ? SPA_IO_IN : 0) |
				(b->queued ? SPA_IO_OUT : 0));
}

static bool queue_output(struct bridge *b, const uint8_t *data, size_t size)
{
	size_t write_pos, first;
	if (size > b->queue_limit - b->queued) {
		fail(b, "PCM transport exceeded bounded jitter capacity");
		return false;
	}
	write_pos = (b->read_pos + b->queued) % QUEUE_SIZE;
	first = SPA_MIN(size, QUEUE_SIZE - write_pos);
	if (data) {
		memcpy(b->queue + write_pos, data, first);
		memcpy(b->queue, data + first, size - first);
	} else {
		memset(b->queue + write_pos, 0, first);
		memset(b->queue, 0, size - first);
	}
	b->queued += size;
	return true;
}

/* Floss's SCO RX packets clock its PCM playback reads. Produce one outgoing
 * sample for each complete incoming sample, using silence if the graph has no
 * playback data. This keeps voice transport alive without a playback client. */
static void voice_playback_credit(struct bridge *b, size_t incoming)
{
	size_t stride = b->channels * 2;
	size_t wanted;
	b->voice_credit += incoming;
	wanted = b->voice_credit - b->voice_credit % stride;
	b->voice_credit -= wanted;
	flush_audio(b);
    if (!b->voice_primed && b->voice_queued >= (size_t)(TARGET_DELAY_SECONDS * b->rate * b->channels * (b->bits / 8)))
        b->voice_primed = true;
    while (wanted && b->voice_primed && b->voice_queued && !b->failed) {
		size_t count = SPA_MIN(wanted, SPA_MIN(b->voice_queued,
				QUEUE_SIZE - b->voice_read_pos));
		if (!queue_output(b, b->voice_queue + b->voice_read_pos, count))
			return;
		b->voice_read_pos = (b->voice_read_pos + count) % QUEUE_SIZE;
		b->voice_queued -= count;
		wanted -= count;
	}
	if (wanted && !b->failed)
		queue_output(b, NULL, wanted);
	flush_audio(b);
}

/* Drop complete microphone samples while there is no capture consumer. Keep
 * a final partial sample: Unix-stream reads need not end on sample boundaries. */
static void discard_idle_capture(struct bridge *b)
{
	size_t stride = b->channels * 2;
	size_t discard = b->capture_queued - b->capture_queued % stride;
	b->capture_read_pos = (b->capture_read_pos + discard) % QUEUE_SIZE;
	b->capture_queued -= discard;
}

static void read_capture(struct bridge *b)
{
	size_t budget = QUEUE_SIZE;
	while (budget && !b->failed) {
		size_t write_pos, count;
		ssize_t n;
		if (!b->capture_running)
			discard_idle_capture(b);
		if (b->capture_queued == b->queue_limit) {
			uint8_t next;
			ssize_t available = recv(b->audio_fd, &next, 1, MSG_DONTWAIT | MSG_PEEK);
			if (available < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
				return;
            if (available <= 0) {
                fail(b, "Floss microphone PCM transport closed or read failed");
                return;
            }
            /* A delayed graph consumer must not disconnect a working SCO
             * link. Keep recent, complete samples and bound capture latency. */
            size_t stride = b->channels * 2;
            size_t keep = (size_t)(TARGET_DELAY_SECONDS * b->rate) * stride;
            size_t drop = (b->capture_queued - keep) / stride * stride;
            b->capture_read_pos = (b->capture_read_pos + drop) % QUEUE_SIZE;
            b->capture_queued -= drop;
            memset(&b->capture_clock, 0, sizeof(b->capture_clock));
            if ((b->capture_overruns++ % 64) == 0)
                fprintf(stderr, "pw-floss: microphone capture overrun; discarded old PCM, keeping transport\n");
		}
		write_pos = (b->capture_read_pos + b->capture_queued) % QUEUE_SIZE;
		count = SPA_MIN(budget, SPA_MIN(b->queue_limit - b->capture_queued,
				QUEUE_SIZE - write_pos));
		n = recv(b->audio_fd, b->capture_queue + write_pos, count, MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return;
		if (n <= 0) {
			fail(b, "Floss microphone PCM transport closed or read failed");
			return;
		}
		b->capture_queued += n;
		budget -= n;
		voice_playback_credit(b, n);
	}
	if (!b->capture_running)
		discard_idle_capture(b);
}

static void audio_ready(void *data, int fd, uint32_t mask)
{
	struct bridge *b = data;
	(void)fd;
	if (mask & (SPA_IO_ERR | SPA_IO_HUP))
		fail(b, "Floss PCM transport disconnected");
	else {
		if (b->hfp && (mask & SPA_IO_IN))
			read_capture(b);
		if (!b->failed && (mask & SPA_IO_OUT))
			flush_audio(b);
	}
}

/* Copy between independently wrapping SPA input and PCM output rings. A
 * source span may end mid-sample; only the full chunk must contain whole frames. */
static void copy_chunk(uint8_t *ring, size_t write_pos, const struct spa_data *data,
        size_t offset, size_t size, bool silent)
{
    while (size) {
        size_t count = SPA_MIN(size, QUEUE_SIZE - write_pos);
        if (!silent) count = SPA_MIN(count, data->maxsize - offset);
        if (silent) memset(ring + write_pos, 0, count);
        else memcpy(ring + write_pos, (const uint8_t *)data->data + offset, count);
        size -= count;
        write_pos = (write_pos + count) % QUEUE_SIZE;
        if (!silent) offset = (offset + count) % data->maxsize;
    }
}

static void process(void *data)
{
    struct bridge *b = data;
    struct pw_buffer *buffer;
    struct spa_data *d;
    size_t offset, size, write_pos;
    bool silent;
    uint64_t now = raw_time_ns();
    double level;

    if (!(buffer = pw_stream_dequeue_buffer(b->stream))) return;
    if (b->failed || b->transitioning || !b->playback_format_ready || buffer->buffer->n_datas == 0) goto done;
    d = &buffer->buffer->datas[0];
    if (!d->chunk) goto done;
    silent = (d->chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0;
    if (!d->maxsize) {
        if (d->chunk->size) fail(b, "nonempty PCM chunk has zero capacity");
        goto done;
    }
    offset = d->chunk->offset % d->maxsize;
    size = SPA_MIN(d->chunk->size, d->maxsize);
    if (size && !silent && !d->data) { fail(b, "PCM payload is not mapped"); goto done; }
    const unsigned sample_bytes = b->bits / 8, frame_bytes = 2 * sample_bytes;
    if (size % frame_bytes) {
        fail(b, "PCM chunk ends in a partial audio frame");
        goto done;
    }
    uint8_t mono[QUEUE_SIZE];
    struct spa_chunk mono_chunk;
    struct spa_data mono_data;
    if (b->channels == 1) {
        if (size / 2 > sizeof(mono)) goto done;
        for (size_t i = 0; i < size / frame_bytes; i++) {
            int64_t sum = 0;
            if (!silent) {
                for (unsigned c = 0; c < 2; c++) {
                    uint32_t word = 0;
                    for (unsigned j = 0; j < sample_bytes; j++)
                        word |= (uint32_t)((uint8_t *)d->data)[(offset + frame_bytes*i + sample_bytes*c + j) % d->maxsize] << (8*j);
                    /* Decode signed LE PCM without alignment assumptions or
                     * shifting negative signed values. */
                    int64_t sample = word;
                    if (word & (UINT32_C(1) << (b->bits - 1))) sample -= INT64_C(1) << b->bits;
                    sum += sample;
                }
            }
            uint32_t value = (uint32_t)(sum / 2);
            for (unsigned j = 0; j < sample_bytes; j++)
                mono[sample_bytes*i+j] = value >> (8*j);
        }
        size /= 2;
        mono_chunk = (struct spa_chunk){ .size = size, .stride = sample_bytes };
        mono_data = (struct spa_data){ .data = mono, .maxsize = sizeof(mono), .chunk = &mono_chunk };
        d = &mono_data;
        offset = 0;
    }
    level = playback_level(b, now);
    adapt_rate(b, b->stream, &b->playback_clock,
            b->hfp && !b->voice_primed ? -1.0 : level, now);
    if (b->failed) goto done;
    if (!b->hfp && level > 0.100) {
        /* Drop this graph quantum until the transport catches up. An xrun
         * must not destroy the device and move all clients to another sink.
         * The position watchdog still detects a permanently stalled peer. */
        goto done;
    }
    if (level >= 0.0) {
        uint64_t delay = (uint64_t)(level * 1e9) +
            (b->hfp ? 10000000u : SPA_MIN(b->remote_delay_ns, (uint64_t)10000000000u));
        report_latency(b, b->stream, false, delay);
    }
    if (b->hfp) {
        if (size > b->queue_limit - b->voice_queued) {
            /* A late graph burst is an xrun, not a device disconnect. */
            goto done;
        }
        write_pos = (b->voice_read_pos + b->voice_queued) % QUEUE_SIZE;
        copy_chunk(b->voice_queue, write_pos, d, offset, size, silent);
        b->voice_queued += size;
    } else {
        flush_audio(b);
        if (b->failed) goto done;
        if (size > b->queue_limit - b->queued) {
            /* Keep the bounded queue and the endpoint on transient overruns. */
            goto done;
        }
        write_pos = (b->read_pos + b->queued) % QUEUE_SIZE;
        copy_chunk(b->queue, write_pos, d, offset, size, silent);
        b->queued += size;
        flush_audio(b);
    }
done:
    pw_stream_queue_buffer(b->stream, buffer);
}

static void capture_process(void *data)
{
	struct bridge *b = data;
	struct pw_buffer *buffer;
	struct spa_data *d;
	size_t stride = 2, frames, size, copied, first;

	if (!(buffer = pw_stream_dequeue_buffer(b->capture)))
		return;
	if (!buffer->buffer->n_datas)
		goto done;
	d = &buffer->buffer->datas[0];
	if (!d->data || !d->chunk)
		goto done;
	frames = d->maxsize / stride;
	if (buffer->requested)
		frames = SPA_MIN(frames, buffer->requested);
    size = frames * stride;
    if (b->hfp && !b->capture_primed && b->capture_queued >= (size_t)(TARGET_DELAY_SECONDS * b->rate * stride))
        b->capture_primed = true;
    if (b->hfp && !b->transitioning) adapt_rate(b, b->capture, &b->capture_clock,
            b->capture_primed ? (double)b->capture_queued / (b->rate * stride) : -1.0,
            raw_time_ns());
    if (b->hfp && !b->transitioning) report_latency(b, b->capture, true, (uint64_t)((double)b->capture_queued / (b->rate * stride) * 1e9));
    copied = b->hfp && !b->transitioning && b->capture_format_ready && b->capture_primed && !b->failed ?
        SPA_MIN(size, b->capture_queued - b->capture_queued % stride) : 0;
	first = SPA_MIN(copied, QUEUE_SIZE - b->capture_read_pos);
	memcpy(d->data, b->capture_queue + b->capture_read_pos, first);
	memcpy((uint8_t *)d->data + first, b->capture_queue, copied - first);
	memset((uint8_t *)d->data + copied, 0, size - copied);
	b->capture_read_pos = (b->capture_read_pos + copied) % QUEUE_SIZE;
	b->capture_queued -= copied;
	d->chunk->offset = 0;
	d->chunk->size = size;
	d->chunk->stride = stride;
	d->chunk->flags = copied ? 0 : SPA_CHUNK_FLAG_EMPTY;
	buffer->size = frames;
done:
	pw_stream_queue_buffer(b->capture, buffer);
}

static void state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct bridge *b = data;
	(void)old;
	b->playback_running = state == PW_STREAM_STATE_STREAMING;
	if (!b->playback_running) {
		b->voice_queued = 0;
        b->voice_read_pos = 0;
        b->voice_primed = false;
        memset(&b->playback_clock, 0, sizeof(b->playback_clock));
        b->position_progress_ns = raw_time_ns();
    }
	if (state == PW_STREAM_STATE_ERROR)
		fail(b, error ? error : "PipeWire stream failed");
	else if (state == PW_STREAM_STATE_UNCONNECTED && !b->starting)
		fail(b, "PipeWire disconnected");
}

static void capture_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct bridge *b = data;
	(void)old;
	b->capture_running = state == PW_STREAM_STATE_STREAMING;

    if (!b->capture_running) {
        discard_idle_capture(b);
        b->capture_primed = false;
        memset(&b->capture_clock, 0, sizeof(b->capture_clock));
    }
    if (state == PW_STREAM_STATE_ERROR)
		fail(b, error ? error : "PipeWire microphone stream failed");
	else if (state == PW_STREAM_STATE_UNCONNECTED && !b->starting)
		fail(b, "PipeWire microphone disconnected");
}

static enum spa_audio_format playback_format(uint8_t bits)
{
    return bits == 32 ? SPA_AUDIO_FORMAT_S32_LE : bits == 24 ? SPA_AUDIO_FORMAT_S24_LE : SPA_AUDIO_FORMAT_S16_LE;
}

static void playback_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct bridge *b = data;
    struct spa_audio_info_raw info = { 0 };
    if (id != SPA_PARAM_Format) return;
    b->repair_layout = param != NULL;
    b->playback_format_ready = param && spa_format_audio_raw_parse(param, &info) >= 0 &&
        info.format == playback_format(b->bits) && info.rate == b->rate && info.channels == 2;
}

static void capture_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct bridge *b = data;
    struct spa_audio_info_raw info = { 0 };
    if (id != SPA_PARAM_Format) return;
    b->capture_format_ready = param && spa_format_audio_raw_parse(param, &info) >= 0 &&
        info.format == SPA_AUDIO_FORMAT_S16_LE && info.rate == b->capture_rate && info.channels == 1;
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = capture_state_changed,
	.process = capture_process,
    .param_changed = capture_param_changed,
};

static void playback_control_info(void *data, uint32_t id, const struct pw_stream_control *control)
{
    struct bridge *b = data;
    if (id == SPA_PROP_channelVolumes && control->n_values == 1)
        b->repair_layout = true;
}

static const struct pw_stream_events events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = state_changed,
    .control_info = playback_control_info,
	.process = process,
    .param_changed = playback_param_changed,
};

static bool connect_stream(struct bridge *b, unsigned adapter, bool capture)
{
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
	struct spa_audio_info_raw format = SPA_AUDIO_INFO_RAW_INIT(
		.format = capture ? SPA_AUDIO_FORMAT_S16_LE : playback_format(b->bits), .rate = capture ? b->capture_rate : b->rate,
        .channels = capture ? 1 : 2);
	const struct spa_pod *params[1];
	struct pw_properties *props;
	struct pw_stream **stream = capture ? &b->capture : &b->stream;
	char node_name[64];
	const char *description = b->device_name[0] ? b->device_name : b->address;

	props = pw_properties_new(PW_KEY_MEDIA_CLASS, capture ? "Audio/Source" : "Audio/Sink",
		PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, capture ? "Capture" : "Playback",
		PW_KEY_NODE_DESCRIPTION, description, "device.api", "floss",
		"device.description", description, "device.bus", "bluetooth",
		"device.icon-name", capture ? "audio-input-microphone-bluetooth" : "audio-headphones-bluetooth",
		"card.profile.device", capture ? "1" : "0",
        "api.floss.profile", b->automatic ? "auto" : b->hfp ? "hfp" : "a2dp",
        "api.floss.active-profile", b->hfp ? "hfp" : "a2dp",
        "priority.session", capture ? "3010" : "2010",
		/* These streams represent physical Bluetooth endpoints. Marking them
		 * virtual hides them in desktop mixers that filter software sinks. */
		"node.pause-on-idle", "true", "node.virtual", "false", "node.want-driver", "true",
        /* This PCM endpoint has no hardware driver to start its graph. Keep
         * playback scheduled so initially corked Pulse clients can negotiate
         * a format before uncorking. Capture must remain demand-driven. */
        "node.always-process", capture ? "false" : "true",
        "resample.disable", "false", NULL);
	if (!props)
		return false;
	snprintf(node_name, sizeof(node_name), "floss_%s.%s", capture ? "input" : "output", b->address);
	for (char *p = node_name; *p; p++)
		if (*p == ':')
			*p = '_';
	pw_properties_set(props, PW_KEY_NODE_NAME, node_name);
	pw_properties_set(props, "api.floss.address", b->address);
    pw_properties_setf(props, "api.floss.adapter", "%u", adapter);
    /* Request 10 ms graph periods so a normal quantum fits comfortably in the
     * bounded 100 ms jitter rings. Forced oversized quanta fail explicitly. */
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", b->rate / 100, b->rate);
    if (b->automatic) pw_properties_setf(props,"device.id","%u",b->card_id);
    *stream = pw_stream_new(b->core, description, props);
	if (!*stream)
		return false;
    pw_stream_add_listener(*stream, capture ? &b->capture_listener : &b->playback_listener,
        capture ? &capture_events : &events, b);
	if (capture) {
		format.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else {
		format.position[0] = SPA_AUDIO_CHANNEL_FL;
		format.position[1] = SPA_AUDIO_CHANNEL_FR;
	}
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
	return pw_stream_connect(*stream, capture ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
		PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) >= 0;
}

/* Keep the playback endpoint stable, but publish capture only when the card
 * profile advertises it. Automatic mode keeps a source for demand detection;
 * fixed music profiles must not offer a nonfunctional microphone to clients. */
static bool sync_capture_profile(struct bridge *b, unsigned profile)
{
    bool wanted = b->automatic ? profile_has_microphone(b, profile) : b->hfp;
    if (!wanted && b->capture) {
        /* Destroy deliberately, without treating UNCONNECTED as a failure. */
        spa_hook_remove(&b->capture_listener);
        pw_stream_destroy(b->capture);
        b->capture = NULL;
        b->capture_running = b->capture_demand = b->capture_format_ready = false;
        b->capture_primed = false;
        b->capture_read_pos = b->capture_queued = 0;
        b->capture_idle_ms = 0;
        memset(&b->capture_clock, 0, sizeof(b->capture_clock));
        b->capture_latency_ns = b->capture_latency_update_ns = 0;
    }
    return !wanted || b->capture || connect_stream(b, b->adapter, true);
}

static void quit(void *data, int signal_number)
{
	(void)signal_number;
    startup_cancelled = 1;
	pw_main_loop_quit(((struct bridge *)data)->main);
}

static bool valid_address(const char *address)
{
	unsigned i;
	if (strlen(address) != 17)
		return false;
	for (i = 0; i < 17; i++)
		if ((i % 3 == 2) ? address[i] != ':' : !isxdigit((unsigned char)address[i]))
			return false;
	return true;
}

/* Snapshot after callback registration closes the enumerate/subscribe race.
 * Keep the selected device until removal; a second headset cannot steal audio. */
static bool discover_device(struct bridge *b)
{
    DBusMessage *reply = call(b, "GetConnectedAudioDevices", DBUS_TYPE_INVALID);
    DBusMessageIter array, devices;
    bool found = false;
    if (!reply || !dbus_message_has_signature(reply, "aa{sv}")) goto done;
    dbus_message_iter_init(reply, &array);
    dbus_message_iter_recurse(&array, &devices);
    while (dbus_message_iter_get_arg_type(&devices) == DBUS_TYPE_ARRAY) {
        DBusMessageIter entries;
        const char *address = NULL, *name = NULL;
        dbus_int32_t hfp = 0;
        int32_t codec_rates[A2DP_CODEC_COUNT] = {0}, codec_modes[A2DP_CODEC_COUNT] = {0}, codec_bits[A2DP_CODEC_COUNT] = {0};
        bool a2dp = false;
        dbus_message_iter_recurse(&devices, &entries);
        while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, value;
            const char *key;
            dbus_message_iter_recurse(&entries, &entry);
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &value);
            int type = dbus_message_iter_get_arg_type(&value);
            if (!strcmp(key, "address") && type == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&value, &address);
            else if (!strcmp(key, "name") && type == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&value, &name);
            else if (!strcmp(key, "hfp_cap") && type == DBUS_TYPE_INT32)
                dbus_message_iter_get_basic(&value, &hfp);
            else if (!strcmp(key, "a2dp_caps") && type == DBUS_TYPE_ARRAY) {
                DBusMessageIter caps;
                dbus_message_iter_recurse(&value, &caps);
                a2dp = dbus_message_iter_get_arg_type(&caps) != DBUS_TYPE_INVALID;
                while (dbus_message_iter_get_arg_type(&caps) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter fields;
                    int32_t codec = -1, rates = 0, modes = 0, bits = 0;
                    dbus_message_iter_recurse(&caps,&fields);
                    while (dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter item, val; const char *field;
                        dbus_message_iter_recurse(&fields,&item);
                        dbus_message_iter_get_basic(&item,&field);
                        dbus_message_iter_next(&item); dbus_message_iter_recurse(&item,&val);
                        if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_INT32) {
                            if (!strcmp(field,"codec_type")) dbus_message_iter_get_basic(&val,&codec);
                            if (!strcmp(field,"sample_rate")) dbus_message_iter_get_basic(&val,&rates);
                            if (!strcmp(field,"channel_mode")) dbus_message_iter_get_basic(&val,&modes);
                            if (!strcmp(field,"bits_per_sample")) dbus_message_iter_get_basic(&val,&bits);
                        }
                        dbus_message_iter_next(&fields);
                    }
                    if (codec >= 0 && codec < A2DP_CODEC_COUNT) {
                        codec_rates[codec] = codec == 4 && (rates & 8) ? 8 :
                            codec == 4 && (rates & 4) ? 4 :
                            codec >= 2 && (rates & 2) ? 2 : rates & 1 ? 1 : rates & 2 ? 2 : 0;
                        codec_bits[codec] = codec == 4 && (bits & 4) ? 4 :
                            codec >= 3 && (bits & 2) ? 2 : bits & 1 ? 1 : 0;
                        codec_modes[codec] = modes & 2 ? 2 : modes & 1 ? 1 : 0;
                    }
                    dbus_message_iter_next(&caps);
                }
            }
            dbus_message_iter_next(&entries);
        }
        if (address && valid_address(address) && (a2dp || hfp) &&
                (!b->address[0] || !strcasecmp(address, b->address))) {
            memcpy(b->codec_rates,codec_rates,sizeof(codec_rates));
            memcpy(b->codec_modes,codec_modes,sizeof(codec_modes));
            memcpy(b->codec_bits,codec_bits,sizeof(codec_bits));
            bool microphone = hfp != 0;
            if (b->address[0] && microphone != b->microphone) {
                fail(b, "headset capabilities changed; refreshing nodes");
                break;
            }
            snprintf(b->address, sizeof(b->address), "%s", address);
            /* D-Bus strings are UTF-8; reject oversized names rather than
             * truncating a multibyte character in a desktop-facing label. */
            snprintf(b->device_name, sizeof(b->device_name), "%s",
                name && strlen(name) < sizeof(b->device_name) ? name : address);
            b->microphone = microphone;
            /* Voice-only devices remain in their only available profile. */
            b->a2dp_available = a2dp;
            found = true;
            break;
        }
        dbus_message_iter_next(&devices);
    }
done:
    if (reply) dbus_message_unref(reply);
    return found;
}

static void stop_transport(struct bridge *b)
{
    const char *owner = CALLBACK_PATH;
    bool stopped;
    if (b->audio_source) {
        pw_loop_destroy_source(pw_main_loop_get_loop(b->main), b->audio_source);
        b->audio_source = NULL;
    }
    if (b->position_call) {
        dbus_pending_call_cancel(b->position_call);
        dbus_pending_call_unref(b->position_call);
        b->position_call = NULL;
    }
    if (b->session) {
        bool_reply(call(b, "StopAudioSession", DBUS_TYPE_UINT64, &b->session,
            DBUS_TYPE_OBJECT_PATH, &owner, DBUS_TYPE_INVALID), &stopped);
        b->session = 0;
    }
    /* Socket closure also requests a native stop. Release the lease first:
     * otherwise the second stop can race the suspend and disconnect A2DP. */
    if (b->audio_fd >= 0) { close(b->audio_fd); b->audio_fd = -1; }
    b->queued = b->capture_queued = b->voice_queued = 0;
    b->capture_overruns = 0;
    b->read_pos = b->capture_read_pos = b->voice_read_pos = b->voice_credit = 0;
    b->voice_primed = b->capture_primed = b->position_valid = false;
    b->audio_written = b->position_bytes = b->position_ticks = 0;
    memset(&b->playback_clock, 0, sizeof(b->playback_clock));
    memset(&b->capture_clock, 0, sizeof(b->capture_clock));
}

static bool update_format(struct pw_stream *stream, uint32_t rate, uint32_t channels, uint8_t bits)
{
    uint8_t storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    struct spa_audio_info_raw format = SPA_AUDIO_INFO_RAW_INIT(
        .format = playback_format(bits), .rate = rate, .channels = channels);
    const struct spa_pod *param;
    format.position[0] = channels == 1 ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_FL;
    if (channels == 2) format.position[1] = SPA_AUDIO_CHANNEL_FR;
    param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    return pw_stream_update_params(stream, &param, 1) >= 0;
}

static void repair_stereo_layout(struct bridge *b)
{
    if (!b->repair_layout || !b->stream) return;
    const struct pw_stream_control *vol = pw_stream_get_control(b->stream,SPA_PROP_channelVolumes);
    if (!vol || !vol->n_values) return;
    b->repair_layout = false;
    if (vol->n_values != 1) return;
    float values[2] = { vol->values[0], vol->values[0] };
    uint32_t map[2] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR };
    uint8_t data[256]; struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data,sizeof(data));
    const struct spa_pod *param = spa_pod_builder_add_object(&builder,SPA_TYPE_OBJECT_Props,SPA_PARAM_Props,
        SPA_PROP_channelMap,SPA_POD_Array(sizeof(uint32_t),SPA_TYPE_Id,2,map),
        SPA_PROP_channelVolumes,SPA_POD_Array(sizeof(float),SPA_TYPE_Float,2,values));
    pw_stream_set_param(b->stream,SPA_PARAM_Props,param);
}
static bool request_codec(struct bridge *b, int profile)
{
    if (profile < 0 || (unsigned)profile >= SPA_N_ELEMENTS(profile_codecs)) return false;
    if (profile_codecs[profile] < 0) return true;
    const char *address = b->address;
    dbus_uint32_t codec = profile_codecs[profile];
    dbus_int32_t rate = b->codec_rates[codec], bits = b->codec_bits[codec], mode = b->codec_modes[codec];
    if (!rate || !bits || !mode) return false;
    int64_t deadline = monotonic_ms() + CALL_TIMEOUT_MS;
    do {
        bool accepted = false;
        if (!bool_reply(call(b,"SetAudioConfig",DBUS_TYPE_STRING,&address,
                DBUS_TYPE_UINT32,&codec,DBUS_TYPE_INT32,&rate,DBUS_TYPE_INT32,&bits,
                DBUS_TYPE_INT32,&mode,DBUS_TYPE_INVALID),&accepted)) return false;
        if (accepted) return true;
        /* Stop acknowledges cancellation before the terminal native callback.
         * Codec configuration is excluded while that lease is still stopping,
         * just like ReserveAudioSession. Keep nodes while waiting, and bound
         * retries so a genuine codec refusal can restore the old profile. */
        dispatch_bus(b);
        if (b->failed || startup_cancelled) return false;
        poll(NULL, 0, 50);
    } while (monotonic_ms() < deadline);
    return false;
}
static void automatic_tick(struct bridge *b)
{
    repair_stereo_layout(b);
    int64_t now = monotonic_ms();
    bool demand = capture_requested(b);
    if (b->capture_demand && !demand) b->capture_idle_ms = now;
    b->capture_demand = demand;
    bool wanted_hfp = !b->a2dp_available || (demand && now >= b->hfp_retry_deadline) ||
        (b->hfp && now - b->capture_idle_ms < 2000);
    if (b->requested_profile) wanted_hfp = b->requested_profile == 3;
    if (b->hfp_transport_unavailable && b->a2dp_available) wanted_hfp = false;
    if (b->transitioning) return;
    if (now >= b->discovery_deadline) {
        b->discovery_deadline = now + 1000;
        if (!discover_device(b)) {
            fail(b, "audio device is no longer ready; waiting for reconnect");
            return;
        }
        if (!b->hfp) {
            struct hfp_pcm_config config;
            if (!get_pcm_config(b, &config) || !config.ready || config.generation != b->generation) {
                fail(b, "A2DP configuration changed; refreshing transport");
                return;
            }
        }
    }
    bool profile_request = b->requested_profile != b->selected_profile;
    if (wanted_hfp == b->hfp && !profile_request) return;
    uint32_t old_rate = b->rate, old_capture_rate = b->capture_rate;
    uint8_t old_bits = b->bits;
    b->transitioning = true;
    /* audioadapter marks EnumFormat changes for renegotiation on restart.
     * Suspend processing, retaining the published nodes and their links. */
    pw_stream_set_active(b->stream, false);
    if (b->capture) pw_stream_set_active(b->capture, false);
    /* Processing is on this same non-RT loop. No PCM callback can race the
     * bounded stop/start RPCs or consume data with the previous format. */
    stop_transport(b);
    int target_profile = b->requested_profile;
    if (profile_request && !request_codec(b,target_profile)) {
        fprintf(stderr,"pw-floss: codec selection refused; restoring previous profile\n");
        target_profile = b->selected_profile;
        b->requested_profile = target_profile;
        wanted_hfp = b->hfp;
    }
    b->hfp = wanted_hfp;
    if (!(b->hfp ? start_hfp(b) : start_audio(b))) {
        /* A rejected SCO request must not remove an application's selected
         * microphone or strand playback. Keep the nodes, restore A2DP, and
         * back off before retrying while capture demand remains. */
        bool fallback = b->hfp && b->a2dp_available && !startup_cancelled;
        stop_transport(b);
        b->failed = false;
        b->hfp = false;
        b->hfp_retry_deadline = monotonic_ms() + 10000;
        target_profile = b->requested_profile = 0;
        if (!fallback || !start_audio(b)) {
            b->transitioning = false;
            fail(b, "profile transition failed; waiting before retry");
            return;
        }
    }
    b->capture_rate = b->hfp ? b->rate : 16000;
    b->queue_limit = b->rate * b->channels * (b->bits / 8) / 10;
    bool playback_changed = old_rate != b->rate || old_bits != b->bits;
    bool capture_changed = old_capture_rate != b->capture_rate;
    if (!sync_capture_profile(b, target_profile)) {
        b->transitioning = false;
        fail(b, "could not publish profile microphone");
        return;
    }
    if (playback_changed) b->playback_format_ready = false;
    if (capture_changed) b->capture_format_ready = false;
    if ((playback_changed && !update_format(b->stream, b->rate, 2, b->bits)) ||
            (b->capture && capture_changed && !update_format(b->capture, b->capture_rate, 1, 16))) {
        b->transitioning = false;
        fail(b, "could not renegotiate PipeWire profile format");
        return;
    }
    b->audio_source = pw_loop_add_io(pw_main_loop_get_loop(b->main), b->audio_fd,
        SPA_IO_ERR | SPA_IO_HUP | (b->hfp ? SPA_IO_IN : 0), false, audio_ready, b);
    if (!b->audio_source) { b->transitioning = false; fail(b, "could not watch new audio transport"); return; }
    queue_output(b, NULL, b->hfp ? b->rate * 2 / 100 :
        (size_t)(TARGET_DELAY_SECONDS * b->rate * b->channels * (b->bits / 8)));
    flush_audio(b);
    pw_stream_set_active(b->stream, true);
    if (b->capture) pw_stream_set_active(b->capture, true);
    struct spa_dict_item active = SPA_DICT_ITEM_INIT("api.floss.active-profile", b->hfp ? "hfp" : "a2dp");
    struct spa_dict properties = SPA_DICT_INIT(&active, 1);
    pw_stream_update_properties(b->stream, &properties);
    if (b->capture) pw_stream_update_properties(b->capture, &properties);
    b->selected_profile = target_profile;
    profile_changed(b);
    b->transitioning = false;
    if (b->failed) pw_main_loop_quit(b->main);
    fprintf(stderr, "pw-floss: automatic %s: %u Hz, %u channel(s)\n",
        b->hfp ? "HFP" : "A2DP", b->rate, b->channels);
}

static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s [--auto] [--adapter N]\n"
        "  Automatically discovers audio devices and selects HFP when its microphone is used.\n"
        "  --device XX:XX:XX:XX:XX:XX --profile a2dp|hfp  diagnostic single session\n"
        "  Floss supplies the negotiated PCM format and owns all Bluetooth codecs.\n", name);
}

static int run_bridge(int argc, char **argv)
{
	struct bridge b = { .audio_fd = -1, .starting = true, .channels = 2, .automatic = true };
	struct pw_loop *loop;
	struct timespec interval = { .tv_nsec = 20 * 1000 * 1000 };
	struct sigaction startup_action = { .sa_handler = cancel_startup };
	unsigned adapter = 0;
	char *end;
	int opt, status = EXIT_FAILURE;
	static const struct option options[] = {
		{ "auto", no_argument, NULL, 'A' },
		{ "device", required_argument, NULL, 'd' },
		{ "profile", required_argument, NULL, 'p' },
		{ "adapter", required_argument, NULL, 'a' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;
	while ((opt = getopt_long(argc, argv, "Ad:p:a:h", options, NULL)) != -1) {
		unsigned long value;
		switch (opt) {
		case 'A': b.automatic = true; break;
        case 'd':
            b.automatic = false;
			if (!valid_address(optarg))
				goto bad_args;
			snprintf(b.address, sizeof(b.address), "%s", optarg);
			break;
		case 'p':
			if (strcmp(optarg, "a2dp") && strcmp(optarg, "hfp"))
				goto bad_args;
			b.hfp = strcmp(optarg, "hfp") == 0;
			break;
		case 'a':
			errno = 0;
			value = strtoul(optarg, &end, 10);
			if (errno || end == optarg || *end || value > 255)
				goto bad_args;
			adapter = value;
			break;
		case 'h':
			usage(argv[0]);
			status = EXIT_SUCCESS;
			goto done;
		default:
			goto bad_args;
		}
	}
	if ((!b.automatic && !b.address[0]) || optind != argc)
		goto bad_args;
	sigemptyset(&startup_action.sa_mask);
	if (sigaction(SIGINT, &startup_action, NULL) < 0 ||
	    sigaction(SIGTERM, &startup_action, NULL) < 0)
		goto done;
	snprintf(b.path, sizeof(b.path), "/org/chromium/bluetooth/hci%u/media", adapter);
    b.adapter = adapter;
    if (!setup_bus(&b)) goto done;
    if (b.automatic) {
        if (!discover_device(&b)) goto done;
        b.hfp = !b.a2dp_available;
    }
    if (!(b.hfp ? start_hfp(&b) : start_audio(&b))) goto done;
    b.capture_rate = b.hfp ? b.rate : 16000;
	b.queue_limit = b.rate * b.channels * (b.bits / 8) / 10;
	if (b.hfp)
		fprintf(stderr, "pw-floss: negotiated HFP PCM: %u Hz S16LE mono\n", b.rate);
	if (startup_cancelled)
		goto done;
	b.main = pw_main_loop_new(NULL);
	if (!b.main)
		goto done;
	loop = pw_main_loop_get_loop(b.main);
	if (!pw_loop_add_signal(loop, SIGINT, quit, &b) ||
	    !pw_loop_add_signal(loop, SIGTERM, quit, &b))
		goto done;
	b.audio_source = pw_loop_add_io(loop, b.audio_fd, SPA_IO_ERR | SPA_IO_HUP | (b.hfp ? SPA_IO_IN : 0),
			false, audio_ready, &b);
	b.bus_timer = pw_loop_add_timer(loop, bus_tick, &b);
	if (!b.audio_source || !b.bus_timer ||
	    pw_loop_update_timer(loop, b.bus_timer, &interval, &interval, false) < 0)
		goto done;
    b.context = pw_context_new(loop, NULL, 0);
    if (!b.context || !(b.core = pw_context_connect(b.context, NULL, 0))) goto done;
    b.card_id = SPA_ID_INVALID;
    if (b.automatic) {
        if (!setup_profiles(&b)) goto done;
        int64_t deadline = monotonic_ms() + 3000;
        while (b.card_id == SPA_ID_INVALID && !startup_cancelled && monotonic_ms() < deadline)
            if (pw_loop_iterate(loop, 20) < 0) break;
        if (b.card_id == SPA_ID_INVALID) goto done;
    }
	if (!connect_stream(&b, adapter, false) || !sync_capture_profile(&b, b.requested_profile))
		goto done;
    if (b.automatic && !setup_capture_watch(&b)) goto done;
    /* Prime transport buffering. HFP graph queues separately prefill to the
     * recovery target while silence keeps the SCO transport clock running. */
    queue_output(&b, NULL, b.hfp ? b.rate * b.channels * 2 / 100 :
            (size_t)(TARGET_DELAY_SECONDS * b.rate * b.channels * (b.bits / 8)));
    flush_audio(&b);
    if (!b.hfp) poll_position(&b);
	b.starting = false;
	if (!b.failed && !startup_cancelled && pw_main_loop_run(b.main) >= 0 && !b.failed)
		status = EXIT_SUCCESS;
	goto done;
bad_args:
	usage(argv[0]);
    status = 64;
done:
	/* All blocking calls occur before streaming or after destroying the node. */
	b.starting = true;
    destroy_capture_watch(&b);
    if (b.card_proxy) pw_proxy_destroy(b.card_proxy);
    if (b.card_properties) pw_properties_free(b.card_properties);
	if (b.capture)
		pw_stream_destroy(b.capture);
	if (b.stream)
		pw_stream_destroy(b.stream);
    if (b.core) pw_core_disconnect(b.core);
    if (b.context) pw_context_destroy(b.context);
	if (b.main) {
		if (b.audio_source)
			pw_loop_destroy_source(pw_main_loop_get_loop(b.main), b.audio_source);
		if (b.bus_timer)
			pw_loop_destroy_source(pw_main_loop_get_loop(b.main), b.bus_timer);
		pw_main_loop_destroy(b.main);
		b.main = NULL;
	}
    if (b.position_call) {
        dbus_pending_call_cancel(b.position_call);
        dbus_pending_call_unref(b.position_call);
        b.position_call = NULL;
    }
    if (b.session && b.bus && dbus_connection_get_is_connected(b.bus)) {
        const char *owner = CALLBACK_PATH;
        bool stopped;
        /* Reservation identity survives cancellation before the first ready
         * snapshot. Stale tokens cannot stop a replacement session. Dropping
         * the authenticated bus owner below also releases a timed-out lease. */
        bool_reply(call(&b, "StopAudioSession", DBUS_TYPE_UINT64, &b.session,
                DBUS_TYPE_OBJECT_PATH, &owner, DBUS_TYPE_INVALID), &stopped);
    }
	if (b.audio_fd >= 0)
		close(b.audio_fd);
	if (b.bus) {
		/* Dropping the bus owner unregisters Floss callbacks. Never Cleanup:
		 * that would tear down media profiles shared with the desktop. */
		dbus_connection_close(b.bus);
		dbus_connection_unref(b.bus);
	}
	free(b.owner);
	return status;
}

int main(int argc, char **argv)
{
    bool automatic = true;
    int status;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") || !strncmp(argv[i], "--device=", 9) ||
                !strcmp(argv[i], "-d") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
            automatic = false;
    }
    pw_init(&argc, &argv);
    do {
        status = run_bridge(argc, argv);
        if (!automatic || startup_cancelled || status == 64) break;
        /* Re-enumerate after device removal, daemon replacement or transport
         * failure. Dropping our bus connection releases every previous lease. */
        for (unsigned i = 0; i < 20 && !startup_cancelled; i++) poll(NULL, 0, 100);
    } while (!startup_cancelled);
    pw_deinit();
    return startup_cancelled ? EXIT_SUCCESS : status;
}
