import QtQuick
import org.kde.kitemmodels as KItemModels
import org.kde.plasma.private.volume
Item {
    SourceModel { id: sources }
    MicrophoneTest { id: mic }
    property int step: 0
    Timer {
        interval: 300; running: true; repeat: true
        onTriggered: {
            const action = step % 3;
            if (action === 0) {
                for (let i = 0; i < sources.rowCount(); ++i) {
                    let source = sources.data(sources.index(i, 0), sources.KItemModels.KRoleNames.role("PulseObject"));
                    if (source.name.startsWith("floss_input.")) mic.source = source;
                }
                if (!mic.source) return;
                mic.startRecording();
            } else if (action === 1) {
                // The former panel-close sequence starts a replay while
                // stopping recording, then disconnects it before it is ready.
                if (LEGACY_CANCEL) {
                    if (mic.recording) mic.stopRecording();
                    if (mic.playing) mic.stopPlaying();
                } else {
                    mic.cleanupStreams();
                }
                mic.clearRecording();
            } else {
                mic.source = null;
            }
            if (++step === 30) { stop(); console.log("DONE"); }
        }
    }
}
