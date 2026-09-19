/* SPDX-License-Identifier: MIT
 * Regression fixture: KDE-style peak meter, deliberately omitting
 * PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND. No audio is recorded to disk. */
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t quit;
static size_t samples;
static void stopped(int signum) { (void)signum; quit = 1; }
static void read_samples(pa_stream *s, size_t length, void *data) {
    (void)length; (void)data;
    const void *p; size_t n;
    if (pa_stream_peek(s, &p, &n) == 0) {
        if (p) samples += n / sizeof(float);
        if (n) pa_stream_drop(s);
    }
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    signal(SIGTERM, stopped); signal(SIGINT, stopped);
    pa_mainloop *m = pa_mainloop_new();
    pa_context *c = pa_context_new(pa_mainloop_get_api(m), "Floss peak meter regression");
    if (!c || pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) return 3;
    time_t deadline = time(NULL) + 8;
    while (pa_context_get_state(c) != PA_CONTEXT_READY) {
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(c)) || time(NULL) > deadline) return 4;
        pa_mainloop_iterate(m, 0, NULL); usleep(1000);
    }
    pa_sample_spec spec = { PA_SAMPLE_FLOAT32NE, 25, 1 };
    pa_stream *s = pa_stream_new(c, "Peak meter", &spec, NULL);
    pa_buffer_attr attr = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, sizeof(float) };
    pa_stream_set_read_callback(s, read_samples, NULL);
    if (pa_stream_connect_record(s, argv[1], &attr,
        PA_STREAM_PEAK_DETECT | PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE) < 0) return 5;
    int ready = 0, failed = 0;
    while (!quit) {
        pa_mainloop_iterate(m, 0, NULL);
        if (!PA_STREAM_IS_GOOD(pa_stream_get_state(s))) { failed = 1; break; }
        if (!ready && pa_stream_get_state(s) == PA_STREAM_READY) {
            puts("meter-ready"); fflush(stdout); ready = 1;
        }
        if (!ready && time(NULL) > deadline) { failed = 1; break; }
        usleep(1000);
    }
    printf("meter-samples=%zu\n", samples);
    pa_stream_disconnect(s); pa_stream_unref(s);
    pa_context_disconnect(c); pa_context_unref(c); pa_mainloop_free(m);
    return failed ? 6 : 0;
}
