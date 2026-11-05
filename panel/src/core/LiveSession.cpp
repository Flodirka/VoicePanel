#include "core/LiveSession.h"
#include "audio/Vad.h"
#include "audio/WasapiCapture.h"

#include <QtMath>

namespace VoicePanel {

namespace {
constexpr int kFrame = Vad::kFrameSamples;   // 512 samples, 32 ms at 16 kHz
}

LiveSession::LiveSession(QObject *parent) : QObject(parent) {
    capture_ = new WasapiCapture(this);
    connect(capture_, &WasapiCapture::framesAvailable, this,
            [this](const QVector<float> &samples, double rms) { onBlock(samples, rms); });
    connect(capture_, &WasapiCapture::error, this, &LiveSession::error);
}

LiveSession::~LiveSession() { stop(); }

void LiveSession::setVadModel(const QString &path) {
    vad_ = std::make_unique<Vad>(path);
}

void LiveSession::configure(int silenceFrames, int minSpeechFrames, int prerollFrames,
                            int maxUtteranceS) {
    silenceFrames_ = qMax(2, silenceFrames);
    minSpeechFrames_ = qMax(1, minSpeechFrames);
    prerollFrames_ = qMax(0, prerollFrames);
    maxUtteranceS_ = qMax(3, maxUtteranceS);
}

bool LiveSession::start() {
    if (running_) return true;
    if (!vad_ || !vad_->valid()) {
        emit error(tr("VAD model not loaded"));
        return false;
    }
    vad_->reset();
    segment_.clear();
    preroll_.clear();
    inSpeech_ = false;
    speechFrames_ = quietFrames_ = 0;
    samplesSeen_ = 0;
    segmentStart_ = 0.0;
    // An empty id means the default endpoint; a stale "default" string is not
    // a device id and used to fail every capture with "device not found".
    capture_->setDevice(device_ == QLatin1String("default") ? QString() : device_);
    capture_->setTargetProcess(processId_);
    const auto source = processId_ != 0
                            ? WasapiCapture::Source::ProcessLoopback
                            : (loopback_ ? WasapiCapture::Source::Loopback
                                         : WasapiCapture::Source::Microphone);
    if (processId_ != 0 && !capture_->start(source)) {
        // Per-application capture can refuse (stale process, driver, policy).
        // Losing the whole session over it is worse than listening to the whole
        // output device, so fall back and say so.
        capture_->setTargetProcess(0);
        capture_->setDevice(QString());
        if (capture_->start(WasapiCapture::Source::Loopback)) {
            running_ = true;
            emit error(tr("Could not isolate that application; listening to the whole output "
                          "device instead."));
            return true;
        }
        emit error(tr("Could not capture call audio"));
        return false;
    }
    if (!capture_->start(source)) {
        emit error(processId_ != 0 ? tr("Could not capture the selected application")
                                   : (loopback_ ? tr("Could not capture system audio")
                                                : tr("Could not start the microphone")));
        return false;
    }
    running_ = true;
    return true;
}

void LiveSession::stop() {
    if (!running_) return;
    running_ = false;
    capture_->stop();
    finishSegment(false);
}

bool LiveSession::active() const { return running_ && capture_->active(); }

void LiveSession::onBlock(const QVector<float> &samples, double rms) {
    if (!running_) return;
    emit level(rms);
    const auto *data = samples.constData();
    int remaining = samples.size();
    while (remaining >= kFrame) {
        const float probability = vad_->probability(data);
        if (!inSpeech_) {
            if (probability >= threshold_) {
                inSpeech_ = true;
                speechFrames_ = 1;
                quietFrames_ = 0;
                segment_ = preroll_;
                segment_.append(QVector<float>(data, data + kFrame));
                segmentStart_ = qMax(0.0, samplesSeen_ / 16000.0 - preroll_.size() / 16000.0);
                preroll_.clear();
                emit speechStarted();
            } else {
                preroll_.append(QVector<float>(data, data + kFrame));
                const int limit = kFrame * prerollFrames_;
                if (preroll_.size() > limit) preroll_.remove(0, preroll_.size() - limit);
            }
        } else {
            segment_.append(QVector<float>(data, data + kFrame));
            if (probability >= threshold_) {
                ++speechFrames_;
                quietFrames_ = 0;
            } else {
                ++quietFrames_;
            }
            const int frames = segment_.size() / kFrame;
            if (quietFrames_ >= silenceFrames_ || frames * kFrame >= maxUtteranceS_ * 16000) {
                finishSegment(true);
            }
        }
        data += kFrame;
        remaining -= kFrame;
        samplesSeen_ += kFrame;
    }
}

void LiveSession::finishSegment(bool emitIt) {
    if (!inSpeech_) {
        segment_.clear();
        return;
    }
    const QVector<float> audio = segment_;
    const double start = segmentStart_;
    const bool enough = speechFrames_ >= minSpeechFrames_;
    segment_.clear();
    preroll_.clear();
    inSpeech_ = false;
    speechFrames_ = quietFrames_ = 0;
    if (emitIt && enough && !audio.isEmpty()) emit utterance(audio, start);
}

} // namespace VoicePanel
