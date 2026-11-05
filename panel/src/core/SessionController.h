#pragma once
#include "core/Settings.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QList>
#include <QObject>
#include <QVector>

class PanelTests;

namespace VoicePanel {

class ServiceClient;
class TtsPlayer;
class LiveSession;

// Drives the two live modes on top of one recogniser:
//
//   Interpreter: system audio -> ASR -> EN->RU -> caption
//                microphone   -> ASR -> RU->EN -> cloned voice into the cable
//   Meeting:     microphone   -> ASR -> "You: ..."
//                system audio -> ASR -> "Others: ..."
//
// Requests are queued and run one at a time: the service serialises inference
// anyway, and a queue keeps the two directions from fighting over the engine.
class SessionController : public QObject {
    Q_OBJECT
public:
    // Subtitles: no microphone, no outgoing translation. One application's
    // audio becomes translated captions (and optionally speech).
    enum class Mode { Interpreter, Meeting, Subtitles };
    // Interpreter outgoing path: translate then speak, speak what was heard
    // unchanged, or leave the call alone and show captions only.
    enum class Outgoing { Translated, Verbatim, Off };

    SessionController(ServiceClient *service, TtsPlayer *tts, QObject *parent = nullptr);
    ~SessionController() override;

    void configure(const Settings &settings, Mode mode);
    // Interpreters sometimes only want the caption; the outgoing voice can be
    // switched off without stopping the session.
    void setOutgoing(Outgoing mode) { outgoing_ = mode; }
    void setSpeakCaptions(bool on) { speakCaptions_ = on; }
    // -1 = follow the setting; 0 = English into Russian, 1 = Russian into English.
    void setDirection(int direction) { direction_ = direction; }
    bool start(const QString &vadModelPath);
    void stop();
    bool running() const { return running_; }
    Mode mode() const { return mode_; }
    QString transcript() const { return transcript_; }

signals:
    // Interpreter captions carry both sides so the window can show the two
    // columns the editor uses: what was heard and what it became.
    void caption(const QString &heard, const QString &translated);
    // The outgoing pair: what the user said and the text that will be voiced
    // into the call. Without it the interpreter only shows the other side.
    void outgoing(const QString &said, const QString &voiced);
    void level(double rms);
    void levelRemote(double rms);
    void line(const QString &speaker, const QString &text, double atSeconds);
    void status(const QString &message);
    void error(const QString &message);
    // A capture failure ends the session: the device pickers must become usable
    // again instead of staying locked behind a dead stream.
    void captureFailed(const QString &message);

private:
    friend class ::PanelTests;
    // One speaker turn parsed out of a diarised transcript
    // ("[0.22][S01] text [4.82][S02] more").
    struct Turn {
        double at = 0.0;
        QString speaker;
        QString text;
    };
    static QVector<Turn> parseDiarized(const QString &text);

    struct Job {
        QVector<float> samples;
        bool incoming = false;
        double at = 0.0;
    };

    void onUtterance(bool incoming, const QVector<float> &samples, double at);
    void pump();
    void transcribeCurrent();
    void finishCurrent(const QString &text);
    void appendTranscript(const QString &speaker, const QString &text, double at);
    // Speaker identity across utterances. Offline diarisation numbers speakers
    // per call, so a second phrase from the same person came back as a new
    // speaker. Each window therefore carries the previous phrase as context,
    // and turns are matched to the already-known ones by temporal overlap.
    struct KnownTurn {
        double start = 0.0;
        double end = 0.0;
        int speaker = 0;
    };
    int canonicalSpeaker(double start, double end);
    static QString speakerLabel(int canonical);
    void startDiarize(std::function<void()> then);
    bool looksLikeBleed(const QString &text) const;
    static QString comparable(const QString &text);

    ServiceClient *service_ = nullptr;
    TtsPlayer *tts_ = nullptr;
    LiveSession *mic_ = nullptr;
    LiveSession *system_ = nullptr;
    Settings settings_;
    Mode mode_ = Mode::Interpreter;
    QString model_;
    QString language_;

    // Two queues, microphone first: waiting behind a busy call makes the
    // user's own sentences arrive seconds late, which reads as a bug.
    QList<Job> micQueue_;
    QList<Job> remoteQueue_;
    Job current_;
    bool hasCurrent_ = false;
    bool busy_ = false;
    bool running_ = false;
    Outgoing outgoing_ = Outgoing::Translated;
    bool speakCaptions_ = false;
    int direction_ = -1;
    QString transcript_;
    // The microphone hears whatever the speakers play. Without echo
    // cancellation that becomes a duplicate, often garbled, "You" line — so a
    // microphone utterance is dropped while call audio is loud, and one whose
    // text repeats a recent remote line is dropped as well.
    qint64 lastRemoteVoiceMs_ = 0;
    QList<QPair<QString, qint64>> recentRemoteText_;
    qint64 lastMicUtteranceAt_ = 0;
    // Remote audio on one contiguous timeline. Diarisation numbers speakers per
    // call, so a window must overlap the previous one for the same voice to keep
    // its number; appending the previous phrase instead of real time made every
    // window a fresh call and produced a new speaker per line.
    QVector<float> remoteTimeline_;
    double timelineStartAt_ = 0.0;   // seconds, session clock
    bool timelineStarted_ = false;
    QList<KnownTurn> knownTurns_;
    int nextSpeaker_ = 1;
    QString pendingSpeaker_;
    QString lastRemoteLine_;
    static constexpr int kDiarizeWindow = 320000;   // 20 s at 16 kHz, the context each window carries
    quint64 generation_ = 0;
};

} // namespace VoicePanel
