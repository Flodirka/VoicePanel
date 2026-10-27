#include "core/DictationController.h"

#include <QBuffer>
#include <QDataStream>
#include <QDateTime>
#include <QtMath>
#include <QPointer>

namespace VoicePanel {

// PCM16 WAV encoder for float32 16 kHz mono samples (declared before use).
static QByteArray encodeWavOf(const QVector<float> &samples);

DictationController::DictationController(ServiceClient *service, QObject *parent)
    : QObject(parent), service_(service) {
    capture_ = new WasapiCapture(this);
    connect(capture_, &WasapiCapture::framesAvailable, this,
            [this](const QVector<float> &samples, double rms) {
                if (!sessionOpen_) return;
                emit levelChanged(rms);
                processBlock(samples);
            });
    connect(capture_, &WasapiCapture::error, this, &DictationController::error);
}

DictationController::~DictationController() { cancel(); }

void DictationController::setVadModel(const QString &onnxPath) {
    vad_ = std::make_unique<Vad>(onnxPath);
}

bool DictationController::start() {
    if (sessionOpen_) return !stopping_;
    if (!vad_ || !vad_->valid()) {
        emit error(tr("VAD model not loaded"));
        return false;
    }
    vad_->reset();
    ++generation_;
    segmentId_ = nextFinalId_ = pendingFinals_ = 0;
    finalResults_.clear();
    deferredFinals_.clear();
    requestedRevision_ = -1;
    stopping_ = false;
    captureDrained_ = false;
    sessionOpen_ = true;
    speculateInFlight_ = false;
    speculativeText_.clear();
    lastSpeculateFrame_ = 0;
    preroll_.clear();
    segment_.clear();
    speechFrames_ = silenceFrames_ = 0;
    inSpeech_ = false;
    revision_ = 0;
    speculativeRevision_ = -1;
    capture_->setDevice(deviceId_);
    if (!capture_->start(WasapiCapture::Source::Microphone)) {
        sessionOpen_ = false;
        emit error(tr("Could not start microphone capture"));
        return false;
    }
    emit sessionStarted();
    return true;
}

void DictationController::stop() {
    if (!sessionOpen_ || stopping_) return;
    stopping_ = true;
    capture_->stop();
    const quint64 generation = generation_;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != generation_) return;
        if (inSpeech_ && speechFrames_ >= params_.minSpeechFrames) finishSegment(true);
        else { inSpeech_ = false; segment_.clear(); }
        captureDrained_ = true;
        maybeEndSession();
    });
}

void DictationController::cancel() {
    const bool wasOpen = sessionOpen_;
    sessionOpen_ = stopping_ = false;
    ++generation_;
    capture_->stop();
    pendingFinals_ = 0;
    finalResults_.clear();
    deferredFinals_.clear();
    speculateInFlight_ = false;
    if (wasOpen) emit sessionEnded(true);
    preroll_.clear();
    segment_.clear();
    inSpeech_ = false;
    speechFrames_ = silenceFrames_ = 0;
    emit partialTranscript({});
}

void DictationController::processBlock(const QVector<float> &samples) {
    const auto *data = samples.constData();
    int remaining = samples.size();
    while (remaining >= kFrame) {
        const float prob = vad_->probability(data);
        if (!inSpeech_) {
            if (prob >= params_.speechThreshold) {
                // speech begins: keep the pre-roll for context
                inSpeech_ = true;
                speechFrames_ = 1;
                silenceFrames_ = 0;
                segment_ = preroll_;
                segment_ += QVector<float>(data, data + kFrame);
                preroll_.clear();
                segmentStartedMs_ = QDateTime::currentMSecsSinceEpoch();
                emit speechStarted();
            } else {
                // maintain pre-roll ring
                preroll_ += QVector<float>(data, data + kFrame);
                const int maxPreroll = kFrame * params_.prerollFrames;
                if (preroll_.size() > maxPreroll)
                    preroll_.remove(0, preroll_.size() - maxPreroll);
            }
        } else {
            segment_ += QVector<float>(data, data + kFrame);
            if (prob >= params_.speechThreshold) {
                ++speechFrames_;
                silenceFrames_ = 0;
                ++revision_;
            } else {
                ++silenceFrames_;
            }
            maybeSpeculate();

            const int framesInSegment = segment_.size() / kFrame;
            const bool enoughSpeech = speechFrames_ >= params_.minSpeechFrames;
            const bool silenceDone = silenceFrames_ >= params_.silenceFrames;
            const bool tooLong = framesInSegment * kFrame >= params_.maxUtteranceS * 16000;
            if (silenceDone || tooLong) {
                finishSegment(enoughSpeech);
            }
        }
        data += kFrame;
        remaining -= kFrame;
    }
}

void DictationController::maybeSpeculate() {
    if (!params_.speculative || !inSpeech_ || speculateInFlight_ || stopping_ || requestedRevision_ == revision_) return;
    const int framesSinceLast = segment_.size() / kFrame - lastSpeculateFrame_;
    const bool pauseCandidate = silenceFrames_ >= 6;
    if (!pauseCandidate && framesSinceLast < params_.speculateEveryFrames) return;
    if (speechFrames_ < params_.minSpeechFrames) return;
    launchSpeculative();
}

void DictationController::launchSpeculative() {
    speculateInFlight_ = true;
    lastSpeculateFrame_ = segment_.size() / kFrame;
    const int revision = revision_;
    requestedRevision_ = revision;
    const int segmentId = segmentId_;
    const quint64 generation = generation_;
    QPointer<DictationController> self(this);
    const QByteArray wav = encodeSegmentWav();
    const QString model = params_.asrModel, language = params_.language;
    // The authoritative finalisation re-requests with the full segment; a
    // speculation whose revision still matches is reused as-is.
    service_->transcribe(wav, model, language,
                         [this, self, generation, segmentId, revision](bool ok, QString text, QString err) mutable {
        if (!self || generation != generation_) return;
        if (deferredFinals_.contains(segmentId)) {
            const auto pending = deferredFinals_.take(segmentId);
            if (ok && !text.trimmed().isEmpty()) emit partialTranscript(text, segmentId);
            if (ok && pending.revision == revision && !text.trimmed().isEmpty())
                deliverFinal(segmentId, text, pending.seconds);
            else requestFinal(segmentId, pending.audio, pending.seconds);
            return;
        }
        if (segmentId != segmentId_) return;
        speculateInFlight_ = false;
        if (!ok) {
            emit statusMessage(tr("Speculative transcription failed: %1").arg(err));
            return;
        }
        speculativeRevision_ = revision;
        speculativeText_ = text;
        if (inSpeech_) emit partialTranscript(text, segmentId);
    });
}

void DictationController::finishSegment(bool emitFinal) {
    const QVector<float> segmentAudio = segment_;
    const int revision = revision_;
    const QString partialSoFar = speculativeText_;
    const int specRevision = speculativeRevision_;
    const bool pendingSpeculation = speculateInFlight_;
    const int index = emitFinal ? segmentId_++ : -1;

    segment_.clear();
    preroll_.clear();
    inSpeech_ = false;
    speechFrames_ = silenceFrames_ = 0;
    revision_ = 0;
    speculativeRevision_ = -1;
    requestedRevision_ = -1;
    speculativeText_.clear();
    speculateInFlight_ = false;
    lastSpeculateFrame_ = 0;
    emit speechEnded();
    if (!emitFinal) return;

    ++pendingFinals_;
    const double speechSeconds = double(segmentAudio.size()) / 16000.0;

    if (specRevision == revision && !partialSoFar.trimmed().isEmpty()) {
        // speculation still covers the final state of the segment
        deliverFinal(index, partialSoFar, speechSeconds);
        return;
    }
    if (pendingSpeculation) {
        deferredFinals_.insert(index, {segmentAudio, revision, speechSeconds});
        return;
    }
    requestFinal(index, segmentAudio, speechSeconds);
}

void DictationController::requestFinal(int index, const QVector<float> &audio, double seconds) {
    const quint64 generation = generation_;
    QPointer<DictationController> self(this);
    const QString model = params_.asrModel, language = params_.language;
    service_->transcribe(encodeWavOf(audio), model, language,
                         [this, self, generation, index, seconds](bool ok, QString text, QString err) mutable {
        if (!self || generation != generation_) return;
        if (!ok) {
            emit error(tr("Transcription failed: %1").arg(err));
            deliverFinal(index, {}, seconds);
            return;
        }
        deliverFinal(index, text, seconds);
    });
}

void DictationController::deliverFinal(int index, const QString &text, double seconds) {
    --pendingFinals_;
    finalResults_.insert(index, {text, seconds});
    while (finalResults_.contains(nextFinalId_)) {
        const int id = nextFinalId_++;
        const auto result = finalResults_.take(id);
        if (!result.first.trimmed().isEmpty()) emit finalTranscript(result.first, result.second, id);
        else emit partialTranscript({}, id);
    }
    maybeEndSession();
}

void DictationController::maybeEndSession() {
    if (stopping_ && captureDrained_ && pendingFinals_ == 0 && !inSpeech_) {
        sessionOpen_ = stopping_ = false;
        emit sessionEnded(false);
    }
}

QByteArray DictationController::encodeSegmentWav() const { return encodeWavOf(segment_); }

static QByteArray encodeWavOf(const QVector<float> &samples) {
    QByteArray wav;
    QBuffer buf(&wav);
    buf.open(QIODevice::WriteOnly);
    QDataStream s(&buf);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("RIFF", 4);
    const quint32 dataSize = quint32(samples.size()) * 2;
    s << quint32(36 + dataSize);
    s.writeRawData("WAVEfmt ", 8);
    s << quint32(16) << quint16(1) << quint16(1) << quint32(16000) << quint32(32000)
      << quint16(2) << quint16(16);
    s.writeRawData("data", 4);
    s << dataSize;
    for (float f : samples) {
        const double v = qBound(-1.0, double(f), 1.0);
        s << qint16(v * 32767.0);
    }
    buf.close();
    return wav;
}

} // namespace VoicePanel
