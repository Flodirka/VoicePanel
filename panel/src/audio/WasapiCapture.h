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
    enum class Source { Microphone, Loopback };
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
    int sampleRate_ = 16000;
    std::atomic_bool running_{false};
};

} // namespace VoicePanel
