#pragma once
#include <QSettings>
#include <QString>

namespace VoicePanel {

// All persistent panel preferences. Backed by QSettings (registry on Windows,
// INI elsewhere); every VAD knob the specification asks for is represented.
struct Settings {
    QString serviceUrl = "http://127.0.0.1:8080";

    // Legacy API keys. The panel uses automatic language detection and one context document.
    QString mode = "auto_correct";
    QString profile = "general";
    QString contextDocument;
    QString context;
    QString asrModel = "default";      // service default | qwen3-asr | whisper
    QString language = "auto";         // auto | ru | en
    QString inputDevice;               // WASAPI capture device id ("" = default)
    QString vadModel;                  // override path to silero_vad.onnx

    // VAD / utterance handling (all exposed in the settings dialog)
    double speechThreshold = 0.5;      // speech-start/end probability threshold
    int silenceFrames = 24;            // 32ms frames of silence to finalise (~0.77 s)
    int minSpeechFrames = 6;           // ignore blips shorter than this
    int prerollFrames = 11;            // kept before speech start (~0.35 s)
    int maxUtteranceS = 30;            // hard cap per utterance
    int speculateEveryFrames = 63;     // speculative transcription cadence (~2 s)
    bool speculative = true;

    // insertion behaviour
    bool quickInsert = true;           // insert automatically on valid target
    bool reviewBeforeInsert = false;   // always open the editor first
    bool autoCleanEditor = true;       // correct completed sentences while capture continues
    int transcriptResetS = 30;         // new transcript after this much silence (0 = never)
    bool autoTranslate = false;
    QString insertHotkey = "Ctrl+Alt+D";   // push-to-dictate
    QString insertHotkey2 = "Ctrl+Alt+R";  // review-mode dictate
    QString saveHotkey = "Ctrl+Alt+S";

    // TTS
    QString ttsModel = "auto";
    QString ttsVoiceProfile;           // cloned voice profile id ("" = built-in)
    QString ttsRussianVoice;           // "builtin" | profile id
    QString ttsEnglishVoice;           // "builtin" | profile id
    double ttsSpeed = 1.0;
    int ttsPauseMs = 0;                // silence added between spoken sentences
    bool ttsEnabled = false;

    // storage
    QString markdownDir;               // empty = disabled
    bool autoSaveSessions = true;
    int audioRetentionDays = 0;        // 0 = keep no audio

    // window
    QByteArray compactGeometry;
    QByteArray editorGeometry;
    QString dockPosition = "right";   // free | top | bottom | left | right

    void load();
    void save() const;
    void loadContext();
    bool saveContext(QString *errorOut = nullptr) const;
};

} // namespace VoicePanel
