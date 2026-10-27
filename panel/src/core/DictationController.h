#pragma once
#include "audio/Vad.h"
#include "audio/WasapiCapture.h"
#include "core/ServiceClient.h"
#include <QObject>
#include <QTimer>
#include <QMap>

class PanelTests;

// Continuous dictation pipeline, Pithagoras-style:
//   capture -> Silero VAD -> pre-roll + segment accumulation ->
//   speculative whole-segment ASR while speaking -> authoritative final ASR.
// The user can keep talking while earlier audio is being recognized.
namespace VoicePanel {

struct DictationParams {
    double speechThreshold = 0.5;
    int silenceFrames = 24;        // 32 ms frames of trailing silence to finalise
    int minSpeechFrames = 6;
    int prerollFrames = 11;
    int maxUtteranceS = 30;
    int speculateEveryFrames = 63; // ~2 s
    bool speculative = true;
    QString asrModel = "default";
    QString language = "auto";
};

class DictationController : public QObject {
    Q_OBJECT
public:
    explicit DictationController(ServiceClient *service, QObject *parent = nullptr);
    ~DictationController() override;

    void setVadModel(const QString &onnxPath);   // (re)loads Silero
    bool vadReady() const { return vad_ && vad_->valid(); }
    void setParams(const DictationParams &p) { params_ = p; }
    DictationParams params() const { return params_; }

    void setInputDevice(const QString &deviceId) { deviceId_ = deviceId; }

    bool start();   // begin a recording session
    void stop();    // finalise pending audio and stop capture
    void cancel();  // drop everything
    bool active() const { return capture_ && capture_->active(); }
    bool pending() const { return inSpeech_ || pendingFinals_ > 0 || speculateInFlight_; }

signals:
    void sessionStarted();
    void sessionEnded(bool cancelled);
    void speechStarted();
    void speechEnded();
    void partialTranscript(const QString &text, int segmentId = 0);
    void finalTranscript(const QString &text, qreal secondsOfSpeech, int segmentId = 0);
    void levelChanged(double rms);                          // audio activity
    void statusMessage(const QString &message);
    void error(const QString &message);

private:
    friend class ::PanelTests;
    void processBlock(const QVector<float> &samples);
    void maybeSpeculate();
    void launchSpeculative();
    void finishSegment(bool emitFinal);
    void deliverFinal(int index, const QString &text, double seconds);
    void requestFinal(int index, const QVector<float> &audio, double seconds);
    void maybeEndSession();
    QByteArray encodeSegmentWav() const;

    ServiceClient *service_;
    WasapiCapture *capture_ = nullptr;
    std::unique_ptr<Vad> vad_;
    DictationParams params_;
    QString deviceId_;

    // segment state (all in 512-sample frames)
    QVector<float> preroll_;
    QVector<float> segment_;          // accumulated segment audio (includes preroll)
    int speechFrames_ = 0;            // frames above threshold in the current segment
    int silenceFrames_ = 0;           // consecutive quiet frames
    bool inSpeech_ = false;
    int revision_ = 0;
    int lastSpeculateFrame_ = 0;
    int speculativeRevision_ = -1;
    int requestedRevision_ = -1;
    QString speculativeText_;
    bool speculateInFlight_ = false;
    qint64 segmentStartedMs_ = 0;
    static constexpr int kFrame = Vad::kFrameSamples;
    quint64 generation_ = 0;
    int segmentId_ = 0;
    int nextFinalId_ = 0;
    int pendingFinals_ = 0;
    bool sessionOpen_ = false;
    bool stopping_ = false;
    bool captureDrained_ = false;
    QMap<int, QPair<QString, double>> finalResults_;
    struct PendingSegment { QVector<float> audio; int revision; double seconds; };
    QMap<int, PendingSegment> deferredFinals_;
};

} // namespace VoicePanel
