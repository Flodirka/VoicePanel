#pragma once
#include "core/Settings.h"
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
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
    // audio becomes translated captions, with the same speaker labels a
    // meeting gets: one stream often carries several voices.
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
    // -1 = follow the setting; 0 = English into Russian, 1 = Russian into English.
    void setDirection(int direction) { direction_ = direction; }
    bool start(const QString &vadModelPath);
    void stop();
    bool running() const { return running_; }
    Mode mode() const { return mode_; }
    QString transcript() const { return transcript_; }

signals:
    // Interpreter captions carry both sides so the window can show the two
    // columns the editor uses: what was heard and what it became. The speaker
    // is empty when nobody has been identified yet (interpreter, first phrase).
    // The id identifies the phrase, so a late speaker label can be applied to
    // the caption it belongs to instead of the newest one.
    void caption(const QString &speaker, const QString &heard, const QString &translated,
                 quint64 id = 0);
    // A caption is emitted as soon as its words are known. The rolling
    // diarisation window answers seconds later, so the resolved label arrives
    // as a correction for that phrase rather than delaying the text.
    void captionSpeaker(quint64 id, const QString &speaker);
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
        quint64 id = 0;          // phrase identity for a late speaker label
    };
    // A phrase waiting for the translator, with the label known when it was
    // captioned; the diariser may replace that label later.
    struct TranslationJob {
        quint64 id = 0;
        QString speaker;
        QString heard;
    };

    void onUtterance(bool incoming, const QVector<float> &samples, double at);
    void pump();
    void transcribeCurrent();
    // Recognition and speaker separation run in separate engine processes, so
    // they are started together and the phrase is finished when both are in.
    void maybeFinishCurrent();
    void finishCurrent(const QString &text);
    // Subtitles translate in their own queue: the recogniser must be free for
    // the next phrase while a sentence waits for the translator.
    void pumpTranslation();
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
    // A person's known speech is kept merged: every window still hears the
    // previous two minutes, so storing its turns again would count the same
    // seconds once per phrase and make an overlap sum measure how often a
    // person was recorded rather than how much of a voice they explain.
    // `windowEnd` is where the diarisation window stops on the session clock;
    // the caller decides it so a backlog is drained forward in overlapping
    // steps instead of jumping to the newest audio.
    void issueDiarize(std::function<void()> then, quint64 phraseId,
                      double phraseStart, double phraseEnd, double windowEnd);
    // The engine takes seconds per window while speech arrives faster, so a
    // window can end far behind the audio already heard. The next window then
    // continues from `coveredEnd_` (in steps of kDiarizeStrideSeconds) instead
    // of from the newest phrase: a jump left the speech between the two windows
    // unlabelled, and the window after it had no overlap with anything known.
    static constexpr double kDiarizeCatchUpSeconds = 10.0;
    // Half the window: consecutive windows keep a minute of shared audio, which
    // is the overlap identity is matched on. A full window of stride would
    // leave every window with no known speech inside it.
    static constexpr double kDiarizeStrideSeconds = 60.0;
    double coveredEnd_ = 0.0;               // audio the windows have labelled
    void recordKnownTurn(int speaker, double start, double end);
    int canonicalSpeaker(double start, double end);
    static QString speakerLabel(int canonical);
    // The voice a live session speaks for one language: an empty row means the
    // voice the user saved, so a recorded clone is what the call hears; only a
    // named profile overrides it.
    static QString sessionVoice(const Settings &settings, const QString &chosen);
    void startDiarize(std::function<void()> then, quint64 phraseId);
    // One caption for one phrase, recording the label it was drawn with so a
    // later diarisation answer can correct only that line.
    void emitCaption(quint64 id, const QString &speaker, const QString &heard,
                     const QString &translated);
    void deliverSpeaker(quint64 id, const QString &speaker);
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
    // A window id belongs to a person when that person's merged spans cover
    // this many seconds of it. Weighing seconds rather than a share matters:
    // the window steps forward between requests, so most of an id's speech is
    // often new and a share would read as "nobody" even for a voice that is
    // clearly the same person. Unrelated voices never overlap in time.
    static constexpr double kDiarizeKnownSeconds = 1.0;
    // The id carrying a phrase holds most of the phrase's speech, and at least
    // this much of it. An id below that is a fragment at a turn boundary:
    // enough to be heard, not enough to be the person the caption is for.
    static constexpr double kDiarizePhraseSeconds = 0.4;
    // A window voice nobody labelled needs this much speech before it is a new
    // participant rather than a fragment the engine cut out of someone else.
    // The quiet voice in a three-way conversation speaks in bursts of this
    // size, so a larger bar left it nameless and its captions sounding like
    // whoever spoke last.
    static constexpr double kDiarizeNewVoiceSeconds = 2.0;
    QString lastRemoteLine_;
    // Speaker and words, kept together: the label is drawn on whichever caption
    // line is visible, and the translation takes a few hundred milliseconds.
    QList<TranslationJob> translationQueue_;
    bool translating_ = false;
    // T-1 merge (experiment): audio of a remote phrase the recogniser returned
    // no words for is held and recognised with the next phrase, so the fragment
    // never becomes a line of its own. `mergedPhrases_` is read by the
    // acceptance run; the fragment keeps no caption.
    // The longest utterance one recognition may carry once a phrase is held;
    // past it the fragment is dropped instead of sending an unbounded request.
    static constexpr double kMergeLimitSeconds = 3.0;
    QVector<float> mergeAudio_;   // held fragments, the silence between them included
    double mergeAt_ = 0.0;        // where mergeAudio_ starts on the session clock
    int mergedPhrases_ = 0;       // fragments actually folded into a later phrase
    int mergePending_ = 0;        // fragments held, not yet folded
    // Phrase identity for captions and their labels. The overlay shows the
    // words at once; the label follows when the diariser answers, so the two
    // are matched by id rather than by being the newest line on screen.
    quint64 nextPhraseId_ = 0;
    QHash<quint64, QString> resolvedSpeakers_;
    QHash<quint64, QString> shownSpeakers_;
    // Where each caption sits on the session clock, so a diarisation that
    // answers late can name the lines it covers, not only the phrase that
    // started it. Only the last few minutes are kept: the window never reaches
    // further back than that.
    struct CaptionSpan {
        double start = 0.0;
        double end = 0.0;
    };
    QHash<quint64, CaptionSpan> captionSpans_;
    static constexpr double kCaptionMemory = 300.0;
    QString lastSpeakerLabel_;
    // One rolling diarisation at a time: it takes seconds, and queueing one
    // per phrase made labels fall further behind than the speech.
    bool diarizing_ = false;
    QString currentText_;
    QString currentError_;
    bool currentRecognized_ = false;
    bool currentDiarized_ = true;
    // Speaker identity survives only while the diariser can still hear the
    // person: its ids are numbered per request. A 20 s window forgot everyone
    // who paused for more than a few seconds and the transcript collected a new
    // "Speaker N" every phrase. 120 s at 16 kHz keeps every voice that took
    // part in the last two minutes, at a fixed 23 MB of audio. A one-minute
    // window is not a substitute: it renumbers the quieter voices, because a
    // person who pauses for a few seconds falls out of it altogether.
    static constexpr int kDiarizeWindow = 1920000;
    quint64 generation_ = 0;
};

} // namespace VoicePanel
