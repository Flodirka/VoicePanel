#pragma once
#include <QObject>
#include <QTimer>
#include <atomic>
#include <memory>

// WASAPI capture: microphone or system-loopback, emitting 16 kHz mono float
// frames. Runs its own capture thread; all signals are queued to the GUI thread.
namespace VoicePanel {

class WasapiCapture : public QObject {
    Q_OBJECT
public:
    // ProcessLoopback captures one application (Windows 10 2004+) without a
    // virtual device, which is how a call is isolated from every other sound.
    enum class Source { Microphone, Loopback, ProcessLoopback };
    explicit WasapiCapture(QObject *parent = nullptr);
    ~WasapiCapture() override;

    // device selection: empty string = system default
    void setDevice(const QString &deviceId);
    void setSampleRate(int rate) { sampleRate_ = rate; }
    QString device() const { return device_; }

    bool start(Source source);
    void stop();
    bool active() const { return running_.load(); }

    // enumerate friendly names; empty string in the result means "default"
    static QList<QPair<QString, QString>> listDevices(bool capture);  // (id, name)
    // Applications that currently render audio (pid, display name).
    static QList<QPair<quint32, QString>> listAudioProcesses();
    // Whether this machine accepts per-application capture at all. The list of
    // applications is only offered when it does: measured here, activation can
    // fail with E_ILLEGAL_METHOD_CALL, and an option that always errors is worse
    // than no option.
    static bool processLoopbackSupported();

    // Target for Source::ProcessLoopback.
    void setTargetProcess(quint32 pid) { processId_ = pid; }

signals:
    void started();
    void stopped();
    // 16 kHz mono float32 samples; size is a multiple of 512 after internal re-framing.
    void framesAvailable(const QVector<float> &samples, double rms);
    void error(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    QString device_;
    quint32 processId_ = 0;
    int sampleRate_ = 16000;
    std::atomic_bool running_{false};
};

} // namespace VoicePanel
