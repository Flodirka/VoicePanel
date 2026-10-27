#include "core/TtsPlayer.h"
#include <QDataStream>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>

#include <windows.h>
#include <mmsystem.h>

using namespace VoicePanel;

namespace {

// Sentences kept in flight while the current one plays. The service serialises
// synthesis per engine, so a deeper queue only adds latency, not throughput.
constexpr int kMaxInFlight = 2;
// A chunk longer than this is cut at a space: a wall of text without sentence
// punctuation must not become one multi-minute request.
constexpr int kMaxChunkChars = 400;
// Silence left at each end of a chunk after trimming (~-48 dBFS counts as
// silence). Two chunks meet with about 240 ms of silence, which reads as a
// normal sentence break; the user setting adds to it.
constexpr int kSilenceMarginMs = 120;
constexpr int kSilenceThreshold = 120;

// Minimal PCM WAV header parse -> (ok, sampleRate, channels, offset, size)
bool parseWav(const QByteArray &wav, int *rate, int *channels, int *offset, int *size) {
    if (wav.size() < 44 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE") return false;
    int pos = 12;
    while (pos + 8 <= wav.size()) {
        const QByteArray id = wav.mid(pos, 4);
        const int len = int(quint8(wav[pos + 4])) | (int(quint8(wav[pos + 5])) << 8) |
                        (int(quint8(wav[pos + 6])) << 16) | (int(quint8(wav[pos + 7])) << 24);
        if (len < 0 || len > wav.size() - pos - 8) return false;
        if (id == "fmt ") {
            if (pos + 8 + 16 > wav.size()) return false;
            const auto u16 = [&](int o) {
                return int(quint8(wav[pos + 8 + o])) | (int(quint8(wav[pos + 9 + o])) << 8);
            };
            const auto u32 = [&](int o) {
                return int(quint8(wav[pos + 8 + o])) | (int(quint8(wav[pos + 9 + o])) << 8) |
                       (int(quint8(wav[pos + 10 + o])) << 16) | (int(quint8(wav[pos + 11 + o])) << 24);
            };
            if (u16(0) != 1) return false;  // PCM only
            *channels = u16(2);
            *rate = u32(4);
        } else if (id == "data") {
            *offset = pos + 8;
            *size = qMin(len, wav.size() - *offset);
            return *rate > 0 && *channels > 0 && *size > 0;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

// waveOut callback: minimal work; hop onto the Qt thread.
void CALLBACK waveOutProcStatic(HWAVEOUT, UINT msg, DWORD_PTR instance, DWORD_PTR, DWORD_PTR) {
    if (msg != WOM_DONE || !instance) return;
    auto *self = reinterpret_cast<TtsPlayer *>(instance);
    QMetaObject::invokeMethod(self, "onWaveDone", Qt::QueuedConnection);
}

} // namespace

QByteArray VoicePanel::trimSilence(const QByteArray &pcm, int rate, int channels, int marginMs) {
    if (rate <= 0 || channels <= 0) return pcm;
    const int frameBytes = 2 * channels;
    const int frames = int(pcm.size()) / frameBytes;
    if (frames <= 0) return pcm;
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const auto loud = [&](int frame) {
        for (int c = 0; c < channels; ++c)
            if (qAbs(int(samples[frame * channels + c])) > kSilenceThreshold) return true;
        return false;
    };
    int first = 0;
    while (first < frames && !loud(first)) ++first;
    if (first == frames) return pcm;   // a quiet clip is not silence: never drop it
    int last = frames - 1;
    while (last > first && !loud(last)) --last;
    const int margin = qMax(0, rate * marginMs / 1000);
    const int start = qMax(0, first - margin);
    const int end = qMin(frames, last + 1 + margin);
    return pcm.mid(start * frameBytes, (end - start) * frameBytes);
}

QByteArray VoicePanel::appendSilence(const QByteArray &pcm, int rate, int channels, int ms) {
    if (ms <= 0 || rate <= 0 || channels <= 0) return pcm;
    const int bytes = (rate * ms / 1000) * 2 * channels;
    if (bytes <= 0) return pcm;
    QByteArray out = pcm;
    out.append(QByteArray(bytes, '\0'));
    return out;
}

TtsPlayer::TtsPlayer(ServiceClient *service, QObject *parent)
    : QObject(parent), service_(service) {}

TtsPlayer::~TtsPlayer() { stop(); }

bool TtsPlayer::hasWork() const {
    return inflight_ > 0 || !queue_.isEmpty() || !ready_.isEmpty() || !pending_.isEmpty();
}

bool TtsPlayer::speaking() const {
    return session_ || hasWork();
}

QStringList TtsPlayer::splitSentences(const QString &text) {
    static const QRegularExpression sentenceBreak(QStringLiteral("(?<=[.!?…])\\s+"));
    static const QRegularExpression words(QStringLiteral("[\\p{L}\\p{N}]+"));
    const auto spokenWords = [](const QString &piece) {
        int count = 0;
        auto matches = words.globalMatch(piece);
        while (matches.hasNext()) {
            matches.next();
            ++count;
        }
        return count;
    };
    QStringList pieces;
    for (const QString &raw : text.split(sentenceBreak, Qt::SkipEmptyParts)) {
        const QString sentence = raw.trimmed();
        if (sentence.isEmpty()) continue;
        if (sentence.size() <= kMaxChunkChars) {
            pieces.append(sentence);
            continue;
        }
        int start = 0;
        while (start < sentence.size()) {
            int end = qMin(start + kMaxChunkChars, int(sentence.size()));
            if (end < sentence.size()) {
                const int space = sentence.lastIndexOf(QLatin1Char(' '), end);
                if (space > start + kMaxChunkChars / 2) end = space;
            }
            pieces.append(sentence.mid(start, end - start).trimmed());
            start = end;
            while (start < sentence.size() && sentence[start].isSpace()) ++start;
        }
    }
    // A phrase of three words or fewer waits for the next one: "Yes." alone
    // would be a separate request and a separate gap.
    QStringList out;
    QString current;
    for (const QString &piece : pieces) {
        current = current.isEmpty() ? piece : current + QLatin1Char(' ') + piece;
        if (spokenWords(current) > 3) {
            out.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty()) {
        if (out.isEmpty()) out.append(current);
        else out.last() += QLatin1Char(' ') + current;
    }
    return out;
}

void TtsPlayer::speak(const QString &text) {
    const QStringList sentences = splitSentences(text);
    if (sentences.isEmpty()) return;
    if (!session_) {
        nextRequest_ = nextChunk_ = 0;
        ordered_.clear();
    }
    queue_ += sentences;
    session_ = true;
    requestMore();
}

void TtsPlayer::stop() {
    ++generation_;
    const bool wasActive = session_ || speaking();
    queue_.clear();
    ready_.clear();
    ordered_.clear();
    inflight_ = 0;
    nextRequest_ = nextChunk_ = 0;
    session_ = false;
    started_ = false;
    closeDevice();
    if (wasActive) emit finished();
}

void TtsPlayer::saveSpeech(const QString &text, const QString &path) {
    const QStringList sentences = splitSentences(text);
    if (sentences.isEmpty()) {
        emit saveFailed(tr("There is no text to speak"));
        return;
    }
    if (path.isEmpty()) {
        emit saveFailed(tr("No file name was given"));
        return;
    }
    saveQueue_ = sentences;
    savePath_ = path;
    savePcm_.clear();
    saveRate_ = saveChannels_ = 0;
    saveNext();
}

void TtsPlayer::saveNext() {
    if (saveQueue_.isEmpty()) {
        writeSavedFile();
        return;
    }
    const QString text = saveQueue_.takeFirst();
    static const QRegularExpression cyrillic(QStringLiteral("[\\x{0400}-\\x{04ff}]"));
    const bool russian = text.contains(cyrillic);
    const QString language = russian ? "ru" : "en";
    const QString model = model_ == "profile" ? QString("auto") : model_;
    const QString voice = languageVoices_ ? (russian ? russianVoice_ : englishVoice_) : voiceProfile_;
    QPointer<TtsPlayer> self(this);
    service_->speech(text, model, voice, speed_,
        [this, self](bool ok, QByteArray wav, QString err) {
            if (!self) return;
            if (!ok) {
                emit saveFailed(err);
                return;
            }
            int rate = 0, channels = 0, offset = 0, size = 0;
            if (!parseWav(wav, &rate, &channels, &offset, &size)) {
                emit saveFailed(tr("TTS returned unparsable audio"));
                return;
            }
            if (saveRate_ == 0) {
                saveRate_ = rate;
                saveChannels_ = channels;
            }
            // One file has one format; a voice that changes it mid-text is cut
            // short rather than written as noise.
            if (rate != saveRate_ || channels != saveChannels_) {
                emit saveFailed(tr("The voice changed its audio format mid-text"));
                return;
            }
            savePcm_ += wav.mid(offset, size);
            saveNext();
        }, language);
}

void TtsPlayer::writeSavedFile() {
    if (savePcm_.isEmpty() || saveRate_ <= 0) {
        emit saveFailed(tr("The service returned no audio"));
        return;
    }
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataSize = quint32(savePcm_.size());
    const quint16 blockAlign = quint16(saveChannels_ * 2);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataSize);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(saveChannels_) << quint32(saveRate_)
        << quint32(saveRate_ * blockAlign) << blockAlign << quint16(16);
    out.writeRawData("data", 4);
    out << dataSize;
    out.writeRawData(savePcm_.constData(), savePcm_.size());

    QSaveFile file(savePath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(wav) != wav.size() || !file.commit()) {
        emit saveFailed(tr("Cannot write %1: %2").arg(savePath_, file.errorString()));
        return;
    }
    emit saved(savePath_);
}

void TtsPlayer::requestMore() {
    while (inflight_ < kMaxInFlight && !queue_.isEmpty()) {
        const QString text = queue_.takeFirst();
        static const QRegularExpression cyrillic(QStringLiteral("[\\x{0400}-\\x{04ff}]"));
        const bool russian = text.contains(cyrillic);
        const QString language = russian ? "ru" : "en";
        const QString model = model_ == "profile" ? QString("auto") : model_;
        const QString voice = languageVoices_ ? (russian ? russianVoice_ : englishVoice_) : voiceProfile_;
        const int id = nextRequest_++;
        ++inflight_;
        const quint64 generation = generation_;
        QPointer<TtsPlayer> self(this);
        service_->speech(text, model, voice, speed_,
            [this, self, generation, id](bool ok, QByteArray wav, QString err) {
                if (!self || generation != generation_) return;
                --inflight_;
                if (!ok) {
                    fail(err);
                    return;
                }
                int rate = 0, channels = 0, offset = 0, size = 0;
                if (!parseWav(wav, &rate, &channels, &offset, &size)) {
                    fail(tr("TTS returned unparsable audio"));
                    return;
                }
                Chunk chunk;
                chunk.rate = rate;
                chunk.channels = channels;
                chunk.pcm = trimSilence(wav.mid(offset, size), rate, channels, kSilenceMarginMs);
                // Playback must keep sentence order even if two replies arrive
                // out of order.
                ordered_.insert(id, chunk);
                while (ordered_.contains(nextChunk_)) {
                    const Chunk next = ordered_.take(nextChunk_++);
                    if (!next.pcm.isEmpty()) ready_.append(next);
                }
                pump();
                requestMore();
            }, language);
    }
}

bool TtsPlayer::deviceMatches(int rate, int channels) const {
    return waveOut_ && rate == deviceRate_ && channels == deviceChannels_;
}

bool TtsPlayer::openDevice(int rate, int channels) {
    closeDevice();
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = WORD(channels);
    fmt.nSamplesPerSec = DWORD(rate);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = WORD(channels * 2);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if (waveOutOpen(reinterpret_cast<HWAVEOUT *>(&waveOut_), WAVE_MAPPER, &fmt,
                    DWORD_PTR(waveOutProcStatic), DWORD_PTR(this),
                    CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        waveOut_ = nullptr;
        return false;
    }
    deviceRate_ = rate;
    deviceChannels_ = channels;
    return true;
}

void TtsPlayer::writeBuffer(const QByteArray &pcm) {
    if (pcm.isEmpty() || !waveOut_) return;
    auto *hdr = new WAVEHDR{};
    pending_.append(PendingBuffer{});
    PendingBuffer &buffer = pending_.last();
    buffer.pcm = pcm;
    buffer.waveHeader = hdr;
    hdr->lpData = buffer.pcm.data();
    hdr->dwBufferLength = DWORD(buffer.pcm.size());
    if (waveOutPrepareHeader(HWAVEOUT(waveOut_), hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        pending_.removeLast();
        delete hdr;
        fail(tr("cannot prepare audio buffer"));
        return;
    }
    if (waveOutWrite(HWAVEOUT(waveOut_), hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(HWAVEOUT(waveOut_), hdr, sizeof(WAVEHDR));
        pending_.removeLast();
        delete hdr;
        fail(tr("cannot play the audio buffer"));
        return;
    }
    if (!started_) {
        started_ = true;
        emit started();
    }
}

void TtsPlayer::pump() {
    while (!ready_.isEmpty()) {
        const Chunk chunk = ready_.first();
        if (!deviceMatches(chunk.rate, chunk.channels)) {
            // A different voice means a different sample rate: let the current
            // buffer finish, then reopen the device. Usually formats match.
            if (waveOut_ && !pending_.isEmpty()) return;
            if (!openDevice(chunk.rate, chunk.channels)) {
                fail(tr("cannot open audio output"));
                return;
            }
        }
        ready_.removeFirst();
        writeBuffer(appendSilence(chunk.pcm, chunk.rate, chunk.channels, pauseMs_));
        if (!session_) return;   // writeBuffer failed and stopped the session
    }
    settle();
}

void TtsPlayer::closeDevice() {
    if (!waveOut_) {
        pending_.clear();
        return;
    }
    waveOutReset(HWAVEOUT(waveOut_));
    for (auto &p : pending_) {
        waveOutUnprepareHeader(HWAVEOUT(waveOut_), reinterpret_cast<WAVEHDR *>(p.waveHeader), sizeof(WAVEHDR));
        delete reinterpret_cast<WAVEHDR *>(p.waveHeader);
    }
    pending_.clear();
    waveOutClose(HWAVEOUT(waveOut_));
    waveOut_ = nullptr;
    deviceRate_ = deviceChannels_ = 0;
}

void TtsPlayer::settle() {
    if (hasWork()) return;
    closeDevice();
    if (!session_) return;
    session_ = false;
    started_ = false;
    emit finished();
}

void TtsPlayer::fail(const QString &message) {
    stop();
    emit error(message);
}

void TtsPlayer::onWaveDone() {
    if (!pending_.isEmpty()) {
        for (int i = pending_.size() - 1; i >= 0; --i) {
            auto *hdr = reinterpret_cast<WAVEHDR *>(pending_[i].waveHeader);
            if (hdr->dwFlags & WHDR_DONE) {
                if (waveOut_) waveOutUnprepareHeader(HWAVEOUT(waveOut_), hdr, sizeof(WAVEHDR));
                delete hdr;
                pending_.removeAt(i);
            }
        }
    }
    if (!pending_.isEmpty()) return;
    pump();
}
