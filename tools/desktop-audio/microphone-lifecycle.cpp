// Exercises the actual patched MicrophoneTest class. Only device discovery and
// translations are replaced; Qt event delivery and PulseAudio streams are real.
#include "microphonetest.h"
#include <PulseAudioQt/Context>
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 64;
    auto *loop = pa_mainloop_new();
    auto *ctx = pa_context_new(pa_mainloop_get_api(loop), "microphone-lifecycle-regression");
    PulseAudioQt::test_context = ctx;
    pa_context_connect(ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
    int result = 1, step = 0, remaining = 0;
    {
        MicrophoneTest test;
        PulseAudioQt::Source *source = nullptr;
        QTimer pump, steps;
        QObject::connect(&pump, &QTimer::timeout, [&] { pa_mainloop_iterate(loop, 0, nullptr); });
        pump.start(1);
        QObject::connect(&steps, &QTimer::timeout, [&] {
            if (pa_context_get_state(ctx) != PA_CONTEXT_READY) return;
            if (step < 30) {
                switch (step % 3) {
                case 0:
                    source = new PulseAudioQt::Source(QString::fromUtf8(argv[1]));
                    test.setSource(source);
                    test.startRecording();
                    break;
                case 1:
                    // Old UI: Stop recording starts playback and Stop playback
                    // runs before PA_STREAM_READY. This used to orphan a stream.
                    test.stopRecording();
                    test.stopPlaying();
                    test.clearRecording();
                    break;
                case 2:
                    delete source; source = nullptr;
                    break;
                }
            } else if (step < 45) {
                switch (step % 3) {
                case 0:
                    source = new PulseAudioQt::Source(QString::fromUtf8(argv[1]));
                    test.setSource(source);
                    test.startRecording();
                    break;
                case 1:
                    // Device vanishes while capture is active.
                    delete source; source = nullptr;
                    if (test.source() || test.recording() || test.playing()) app.exit(2);
                    break;
                case 2: test.cleanupStreams(); break;
                }
            } else if (step == 45) {
                // UI object destroyed while playback is still being created.
                auto *temporary = new MicrophoneTest;
                source = new PulseAudioQt::Source(QString::fromUtf8(argv[1]));
                temporary->setSource(source);
                temporary->startRecording();
                QTimer::singleShot(100, temporary, [temporary] {
                    temporary->stopRecording();
                    delete temporary;
                });
            } else if (step == 48) {
                delete source;
                source = new PulseAudioQt::Source(QString::fromUtf8(argv[1]));
                test.setSource(source);
                test.startRecording();
            } else if (step == 49) {
                if (!test.hasRecording()) { app.exit(5); return; }
                test.stopRecording(); // Let playback drain naturally this time.
            } else if (step == 60) {
                if (test.playing() || test.recording()) { app.exit(6); return; }
                delete source; source = nullptr;
                steps.stop();
                struct Counts { QCoreApplication *app; int *result; int *remaining; };
                static Counts counts{&app, &result, &remaining};
                auto *op = pa_context_get_sink_input_info_list(ctx, [](pa_context *, const pa_sink_input_info *info, int eol, void *data) {
                    auto *c = static_cast<Counts *>(data);
                    if (eol) {
                        *c->result = eol < 0 || *c->remaining ? 1 : 0;
                        std::printf("remaining microphone replays: %d\n", *c->remaining);
                        c->app->exit(*c->result);
                    } else if (info && info->name && std::strcmp(info->name, "MicTest-Playback") == 0) {
                        ++*c->remaining;
                    }
                }, &counts);
                if (op) pa_operation_unref(op); else app.exit(3);
            }
            ++step;
        });
        steps.start(200);
        QTimer::singleShot(20000, &app, [&] { app.exit(4); });
        result = app.exec();
    }
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(loop);
    return result;
}
