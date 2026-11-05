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
    QString asrModel = "nemotron";     // nemotron everywhere; qwen3-asr | parakeet switchable
    QString meetingModel = "nemotron-diar";  // task: diar, independent of ASR
    bool meetingDiarize = true;        // label remote speakers when available
    bool meetingAutoSave = true;       // write the transcript on stop, as .md
    QString meetingTranscriptDir;      // empty -> Documents/VoicePanel/Meetings
    int subtitlesDisplay = 0;          // 0 = transcript+translation, 1 = translation, 2 = transcript
    int subtitlesTextSize = 1;         // 0 = S, 1 = M, 2 = L
    int meetingLanguage = 0;           // 0 auto/mixed | 1 Russian | 2 English
    int liveOutgoing = 0;              // 0 translated | 1 verbatim | 2 captions only
    int callLanguage = 0;              // 0 the other side speaks English | 1 Russian
    bool speakCaptions = false;        // subtitles overlay also speaks the translation
    int subtitlesLanguage = 0;         // 0 the captioned audio is English -> Russian | 1 the reverse
    QString language = "auto";         // auto | ru | en
    QString inputDevice;               // WASAPI capture device id ("" = default)
    QString systemAudioDevice;         // render endpoint id captured as loopback ("" = default)
    int outputDevice = -1;             // waveOut index for speech (virtual cable); -1 = default
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
    QString markdownDir;               // empty = local default when auto-save is enabled
    bool autoSaveSessions = true;
    int audioRetentionDays = 0;        // 0 = keep no audio

    // window
    QByteArray compactGeometry;
    QByteArray editorGeometry;
    QString dockPosition = "right";   // free | top | bottom | left | right

    void load();
    void save() const;
    QString transcriptDirectory() const;
    void loadContext();
    bool saveContext(QString *errorOut = nullptr) const;

    // Terms listed under a "Terminology" (Терминология / Glossary) heading in
    // the context document: one per line, "- " bullets allowed. They are sent
    // to the service as the request dictionary, so near-miss ASR renderings of
    // them are fixed deterministically before the LLM runs (see
    // voiceservice.processing.deterministic_pass) and the Windows spell
    // checker does not flag them.
    static QStringList termsFromContext(const QString &context);
};

} // namespace VoicePanel
