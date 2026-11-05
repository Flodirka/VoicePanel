#pragma once
#include <QObject>
#include <QVector>
#include <memory>

namespace VoicePanel {

class WasapiCapture;
class Vad;

// One audio source (microphone or system loopback) split into utterances.
//
// The dictation controller keeps its own copy of this logic because it also
// drives speculative previews; the interpreter and meeting modes only need
// "here is one finished utterance and when it started", so this class stays
// deliberately small and synchronous with the capture thread's blocks.
class LiveSession : public QObject {
    Q_OBJECT
public:
    explicit LiveSession(QObject *parent = nullptr);
    ~LiveSession() override;

    void setVadModel(const QString &path);
    void setDevice(const QString &deviceId) { device_ = deviceId; }
    void setLoopback(bool loopback) { loopback_ = loopback; }
    // Capture one application instead of a device; 0 means "use the device".
    void setTargetProcess(quint32 pid) { processId_ = pid; }
    void configure(int silenceFrames, int minSpeechFrames, int prerollFrames, int maxUtteranceS);
    // Same knob dictation uses; a different threshold segments the same speech
    // into different utterances, which is why the two paths disagreed.
    void setSpeechThreshold(double threshold) { threshold_ = qBound(0.05, threshold, 0.95); }

    bool start();
    void stop();
    bool active() const;
    double elapsedSeconds() const { return samplesSeen_ / 16000.0; }

signals:
    // samples: utterance audio at 16 kHz; startSeconds: offset in the session
    void utterance(const QVector<float> &samples, double startSeconds);
    void speechStarted();
    void level(double rms);
    void error(const QString &message);

private:
    void onBlock(const QVector<float> &samples, double rms);
    void finishSegment(bool emitIt);

    WasapiCapture *capture_ = nullptr;
    std::unique_ptr<Vad> vad_;
    QString device_;
    bool loopback_ = false;
    quint32 processId_ = 0;

    int silenceFrames_ = 24;
    int minSpeechFrames_ = 6;
    int prerollFrames_ = 11;
    int maxUtteranceS_ = 30;
    double threshold_ = 0.5;

    QVector<float> segment_;
    QVector<float> preroll_;
    bool inSpeech_ = false;
    int speechFrames_ = 0;
    int quietFrames_ = 0;
    qint64 samplesSeen_ = 0;
    double segmentStart_ = 0.0;
    bool running_ = false;
};

} // namespace VoicePanel
