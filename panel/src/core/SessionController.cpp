#include "core/SessionController.h"
#include "core/LiveSession.h"
#include "core/ServiceClient.h"
#include "core/TtsPlayer.h"

#include <QDataStream>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QDateTime>
#include <QPointer>

namespace VoicePanel {

namespace {

QByteArray encodeWav(const QVector<float> &samples) {
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataSize = quint32(samples.size()) * 2;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataSize);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(16000) << quint32(32000)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << dataSize;
    for (float value : samples) {
        const double clamped = qBound(-1.0, double(value), 1.0);
        out << qint16(clamped * 32767.0);
    }
    return wav;
}

QString clock(double seconds) {
    const int total = int(seconds);
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

SessionController::SessionController(ServiceClient *service, TtsPlayer *tts, QObject *parent)
    : QObject(parent), service_(service), tts_(tts) {
    mic_ = new LiveSession(this);
    system_ = new LiveSession(this);
    system_->setLoopback(true);   // process capture overrides this when a pid is set

    connect(mic_, &LiveSession::utterance, this,
            [this](const QVector<float> &samples, double at) { onUtterance(false, samples, at); });
    connect(system_, &LiveSession::utterance, this,
            [this](const QVector<float> &samples, double at) { onUtterance(true, samples, at); });
    connect(mic_, &LiveSession::error, this,
            [this](const QString &message) { emit captureFailed(message); });
    connect(mic_, &LiveSession::level, this, &SessionController::level);
    connect(system_, &LiveSession::level, this, &SessionController::levelRemote);
    connect(system_, &LiveSession::level, this, [this](double rms) {
        if (rms > 0.015) lastRemoteVoiceMs_ = QDateTime::currentMSecsSinceEpoch();
    });
    // Barge-in: the moment the user starts a new sentence, the interpreter
    // stops talking. Without this the caller hears the previous translation
    // over the beginning of the next one.
    connect(mic_, &LiveSession::speechStarted, this, [this] {
        if (tts_ && tts_->speaking()) tts_->stop();
    });
    connect(system_, &LiveSession::error, this,
            [this](const QString &message) { emit captureFailed(message); });
}

SessionController::~SessionController() { stop(); }

void SessionController::configure(const Settings &settings, Mode mode) {
    settings_ = settings;
    mode_ = mode;
    model_ = settings.asrModel == QLatin1String("default") ? QString() : settings.asrModel;
    language_ = settings.language == QLatin1String("auto") ? QString() : settings.language;
    for (LiveSession *session : {mic_, system_}) {
        session->configure(settings.silenceFrames, settings.minSpeechFrames,
                           settings.prerollFrames, settings.maxUtteranceS);
        session->setSpeechThreshold(settings.speechThreshold);
    }
    if (tts_) {
        tts_->setModel(settings.ttsVoiceProfile.isEmpty() ? settings.ttsModel : QStringLiteral("auto"));
        tts_->setVoiceProfile(settings.ttsVoiceProfile);
        tts_->setLanguageVoices(settings.ttsRussianVoice, settings.ttsEnglishVoice);
        tts_->setSpeed(settings.ttsSpeed);
        tts_->setPauseMs(settings.ttsPauseMs);
    }
}

bool SessionController::start(const QString &vadModelPath) {
    if (running_) return true;
    transcript_.clear();
    micQueue_.clear();
    remoteQueue_.clear();
    hasCurrent_ = false;
    busy_ = false;
    ++generation_;

    mic_->setVadModel(vadModelPath);
    system_->setVadModel(vadModelPath);
    mic_->setDevice(settings_.inputDevice);
    // The call source is either one application ("pid:1234", captured without a
    // virtual device) or a render endpoint captured as loopback; empty = default.
    if (settings_.systemAudioDevice.startsWith(QLatin1String("pid:"))) {
        system_->setTargetProcess(settings_.systemAudioDevice.mid(4).toUInt());
        system_->setDevice(QString());
    } else {
        system_->setTargetProcess(0);
        system_->setDevice(settings_.systemAudioDevice);
    }

    if (mode_ != Mode::Subtitles && !mic_->start()) return false;
    if (mode_ == Mode::Interpreter || mode_ == Mode::Meeting || mode_ == Mode::Subtitles) {
        if (!system_->start()) {
            emit error(tr("System audio could not be captured; only the microphone is running. "
                          "Pick another output device in Settings."));
        }
    }
    running_ = true;
    emit status(mode_ == Mode::Interpreter ? tr("Interpreting") : tr("Recording the meeting"));
    return true;
}

void SessionController::stop() {
    if (!running_) return;
    running_ = false;
    ++generation_;
    mic_->stop();
    system_->stop();
    micQueue_.clear();
    remoteQueue_.clear();
    hasCurrent_ = false;
    busy_ = false;
    if (tts_) tts_->stop();
    emit status(tr("Stopped"));
}

void SessionController::onUtterance(bool incoming, const QVector<float> &samples, double at) {
    if (!running_) return;
    if (samples.size() < 4000) return;          // <250 ms: a cough, not a phrase
    if (!incoming) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (lastRemoteVoiceMs_ > 0 && now - lastRemoteVoiceMs_ < 1200) {
            return;   // the microphone is hearing the speakers, not the user
        }
    }
    (incoming ? remoteQueue_ : micQueue_).append(Job{samples, incoming, at});
    pump();
}

void SessionController::pump() {
    if (busy_ || !running_) return;
    if (micQueue_.isEmpty() && remoteQueue_.isEmpty()) return;
    current_ = micQueue_.isEmpty() ? remoteQueue_.takeFirst() : micQueue_.takeFirst();
    hasCurrent_ = true;
    busy_ = true;
    transcribeCurrent();
}

void SessionController::transcribeCurrent() {
    const QPointer<SessionController> self(this);
    const quint64 generation = generation_;
    const QByteArray wav = encodeWav(current_.samples);
    // Remote meeting audio can be diarised: one model call returns speaker
    // turns plus timestamps, which is the only way to tell participants
    // apart when they share one stream.
    QString model = model_;
    QString language = language_;
    if (mode_ == Mode::Meeting) {
        if (settings_.meetingLanguage == 2) language = QStringLiteral("en");
        else if (settings_.meetingLanguage == 1) language = QStringLiteral("ru");
    } else if (mode_ == Mode::Interpreter) {
        const bool remoteEnglish = settings_.callLanguage == 0;
        language = (current_.incoming == remoteEnglish) ? QStringLiteral("en") : QStringLiteral("ru");
    } else if (mode_ == Mode::Subtitles) {
        language = settings_.subtitlesLanguage == 0 ? QStringLiteral("en") : QStringLiteral("ru");
    }
    // Remote meeting speech is labelled by a separate diariser, so the words can
    // come from whichever recogniser handles the meeting language best.
    const bool needsDiarize = mode_ == Mode::Meeting && current_.incoming;
    auto recognize = [this, self, generation, wav, model, language] {
        if (!self || generation != generation_) return;
        service_->transcribe(wav, model, language,
                             [this, self, generation](bool ok, QString text, QString error) {
            if (!self || generation != generation_) return;
            if (!ok) {
                busy_ = false;
                hasCurrent_ = false;
                emit this->error(tr("Recognition failed: %1").arg(error));
                pump();
                return;
            }
            finishCurrent(text.trimmed());
        });
    };
    if (needsDiarize) startDiarize(recognize);
    else recognize();
}

void SessionController::finishCurrent(const QString &text) {
    const Job job = current_;
    hasCurrent_ = false;
    if (text.isEmpty()) {
        busy_ = false;
        pump();
        return;
    }

    if (mode_ == Mode::Meeting) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (job.incoming) {
            recentRemoteText_.append({comparable(text), now});
            while (!recentRemoteText_.isEmpty() && now - recentRemoteText_.first().second > 12000) {
                recentRemoteText_.removeFirst();
            }
            const auto turns = parseDiarized(text);
            QStringList speakers;
            QStringList pieces;
            QList<double> offsets;
            if (!turns.isEmpty()) {
                for (const Turn &turn : turns) {
                    speakers << turn.speaker;
                    pieces << turn.text;
                    offsets << job.at + turn.at;
                }
            } else {
                speakers << (pendingSpeaker_.isEmpty() ? tr("Others") : pendingSpeaker_);
                pendingSpeaker_.clear();
                pieces << text;
                offsets << job.at;
            }
            // Meeting text goes through the same correction as dictation; raw
            // recognition has no punctuation or casing, which read as "worse
            // quality" next to the dictation preview.
            const QPointer<SessionController> self(this);
            const quint64 generation = generation_;
            // The service only knows correct_en / correct_ru; a bare "correct"
            // was rejected with 400, so the text stayed raw.
            const QString correctionMode =
                QRegularExpression(QStringLiteral("[\\p{Cyrillic}]")).match(text).hasMatch()
                ? QStringLiteral("correct_ru") : QStringLiteral("correct_en");
            service_->processText(text, correctionMode, settings_.profile,
                                  lastRemoteLine_, {},
                                  [this, self, generation, speakers, pieces, offsets](
                                      bool ok, QString result, QString, QString) {
                if (!self || generation != generation_) return;
                // Correction may return a kept-as-is line or a failure; either way
                // the recognised text is what belongs in the transcript.
                const QString corrected = ok ? result.trimmed() : QString();
                for (int i = 0; i < speakers.size(); ++i) {
                    QString text = pieces[i];
                    if (i == pieces.size() - 1 && !corrected.isEmpty()) text = corrected;
                    appendTranscript(speakers[i], text, offsets[i]);
                    emit line(speakers[i], text, offsets[i]);
                    lastRemoteLine_ = text;
                }
                busy_ = false;
                pump();
            });
            return;
        } else {
            if (looksLikeBleed(text)) {          // the microphone heard the speakers
                busy_ = false;
                pump();
                return;
            }
            appendTranscript(tr("You"), text, job.at);
            emit line(tr("You"), text, job.at);
        }
        busy_ = false;
        pump();
        return;
    }

    // Interpreter. Incoming speech always becomes a Russian caption; the
    // outgoing path depends on the chosen mode.
    if (job.incoming) {
        const QPointer<SessionController> self(this);
        const quint64 generation = generation_;
        const int direction = direction_ >= 0 ? direction_ : settings_.callLanguage;
        const QString intoMine = direction == 1 ? QStringLiteral("translate_ru_en")
                                                       : QStringLiteral("translate_en_ru");
        service_->processText(text, intoMine, settings_.profile, QString(), {},
                              [this, self, generation, text](bool ok, QString result, QString, QString error) {
            if (!self || generation != generation_) return;
            busy_ = false;
            if (!ok) {
                emit this->error(tr("Translation failed: %1").arg(error));
            } else {
                emit caption(text, result.trimmed());
                if (speakCaptions_ && tts_) tts_->speak(result.trimmed());
            }
            pump();
        });
        return;
    }

    if (outgoing_ == Outgoing::Off) {
        emit outgoing(text, QString());
        busy_ = false;
        pump();
        return;
    }
    if (outgoing_ == Outgoing::Verbatim) {
        emit outgoing(text, text);
        if (tts_) tts_->speak(text);
        busy_ = false;
        pump();
        return;
    }

    const QPointer<SessionController> self(this);
    const quint64 generation = generation_;
    const QString intoTheirs = settings_.callLanguage == 1 ? QStringLiteral("translate_en_ru")
                                                          : QStringLiteral("translate_ru_en");
    service_->processText(text, intoTheirs, settings_.profile, QString(), {},
                          [this, self, generation, text](bool ok, QString result, QString, QString error) {
        if (!self || generation != generation_) return;
        busy_ = false;
        if (!ok) {
            emit this->error(tr("Translation failed: %1").arg(error));
        } else {
            emit outgoing(text, result.trimmed());
            if (tts_ && outgoing_ == Outgoing::Translated) tts_->speak(result.trimmed());
        }
        pump();
    });
}


QVector<SessionController::Turn> SessionController::parseDiarized(const QString &text) {
    QVector<Turn> turns;
    // MOSS-Transcribe-Diarize emits "[start][Sxx] words" per turn. Anything it
    // does not tag is not a turn and is ignored here.
    static const QRegularExpression pattern(
        QStringLiteral("\\[(\\d+(?:\\.\\d+)?)\\]\\[(S\\d+)\\]([^\\[]*)"));
    auto it = pattern.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString body = match.captured(3).trimmed();
        if (body.isEmpty()) continue;
        Turn turn;
        turn.at = match.captured(1).toDouble();
        turn.speaker = tr("Speaker %1").arg(match.captured(2).mid(1).toInt());
        turn.text = body;
        turns.append(turn);
    }
    return turns;
}

QString SessionController::comparable(const QString &text) {
    QString folded = text.toLower();
    folded.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
    return folded.simplified();
}

bool SessionController::looksLikeBleed(const QString &text) const {
    const QString probe = comparable(text);
    if (probe.isEmpty()) return true;
    const QStringList words = probe.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &entry : recentRemoteText_) {
        if (now - entry.second > 8000) continue;
        const QStringList remote = entry.first.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (remote.isEmpty()) continue;
        int common = 0;
        for (const QString &word : words) {
            if (remote.contains(word)) ++common;
        }
        if (double(common) / double(qMax(words.size(), remote.size())) >= 0.6) return true;
    }
    return false;
}

int SessionController::canonicalSpeaker(double start, double end) {
    // Same person, next phrase: the most overlapping known turn wins. Offline
    // diarisation numbers speakers per call, so without this every phrase would
    // restart at "Speaker 1". A phrase usually starts after a pause, so the
    // match allows a small gap around each known turn — otherwise a speaker who
    // merely pauses between phrases would get a new number every time.
    static constexpr double kSpeakerGap = 2.0;   // seconds of tolerated silence
    double best = 0.0;
    int matched = 0;
    for (const KnownTurn &known : knownTurns_) {
        const double overlap =
            qMin(end, known.end + kSpeakerGap) - qMax(start, known.start - kSpeakerGap);
        // >= so the most recent turn wins ties: with two alternating speakers
        // the later one is the better guess for what follows.
        if (overlap > 0.0 && overlap >= best) {
            best = overlap;
            matched = known.speaker;
        }
    }
    if (!matched) matched = nextSpeaker_++;
    knownTurns_.append({start, end, matched});
    while (knownTurns_.size() > 400) knownTurns_.removeFirst();
    return matched;
}

void SessionController::startDiarize(std::function<void()> then) {
    const QVector<float> samples = current_.samples;
    const double at = current_.at;          // seconds on the session clock
    if (samples.isEmpty()) {
        then();
        return;
    }
    // Place the phrase on the meeting timeline, filling silence with zeros so
    // every window covers real time. Times here are seconds, not milliseconds.
    if (!timelineStarted_) {
        timelineStarted_ = true;
        timelineStartAt_ = at;
    }
    const int offset = int((at - timelineStartAt_) * 16000.0);
    if (offset < 0) {
        then();
        return;
    }
    if (remoteTimeline_.size() < offset) remoteTimeline_.resize(offset, 0.0f);
    remoteTimeline_ += samples;
    while (remoteTimeline_.size() > kDiarizeWindow * 3) {
        const int drop = remoteTimeline_.size() - kDiarizeWindow * 3;
        remoteTimeline_.remove(0, drop);
        timelineStartAt_ += double(drop) / 16000.0;
    }
    if (!settings_.meetingDiarize || !service_) {
        then();
        return;
    }
    const double phraseStart = at - timelineStartAt_;
    const double phraseEnd = phraseStart + double(samples.size()) / 16000.0;
    const int windowStart = qMax(0, remoteTimeline_.size() - kDiarizeWindow);
    // The buffer ends with this phrase, so the window start is measured back
    // from the phrase END. Measuring from its start shifted every turn earlier
    // by the phrase's own length, so turns fell outside the phrase and were
    // dropped — every remote line came back as "Others".
    const double windowBase = phraseEnd - double(remoteTimeline_.size() - windowStart) / 16000.0;
    const QVector<float> window = remoteTimeline_.mid(windowStart);
    const QPointer<SessionController> self(this);
    const quint64 generation = generation_;
    service_->diarize(encodeWav(window),
                      [this, self, generation, phraseStart, phraseEnd, windowBase, then](bool ok,
                                                                             QJsonArray turns,
                                                                             QString error) {
        if (!self || generation != generation_) return;
        if (ok && !turns.isEmpty()) {
            QMap<int, double> spoken;
            for (const QJsonValue &value : turns) {
                const QJsonObject turn = value.toObject();
                const double start = windowBase
                    + turn.value(QStringLiteral("start_sample")).toDouble() / 16000.0;
                const double end = windowBase
                    + turn.value(QStringLiteral("end_sample")).toDouble() / 16000.0;
                if (end <= phraseStart + 0.05 || start >= phraseEnd + 0.05 || end <= start)
                    continue;
                const int canonical = canonicalSpeaker(start, end);
                spoken[canonical] += qMax(0.0, qMin(end, phraseEnd) - qMax(start, phraseStart));
            }
            int dominant = 0;
            double longest = 0.0;
            for (auto it = spoken.constBegin(); it != spoken.constEnd(); ++it) {
                if (it.value() > longest) {
                    longest = it.value();
                    dominant = it.key();
                }
            }
            if (dominant) pendingSpeaker_ = tr("Speaker %1").arg(dominant);
        } else if (!ok && !error.isEmpty()) {
            emit this->error(tr("Speaker separation failed: %1").arg(error));
        }
        then();
    });
}

void SessionController::appendTranscript(const QString &speaker, const QString &text, double at) {
    const QString line = QStringLiteral("[%1] %2: %3\n").arg(clock(at), speaker, text);
    transcript_ += line;
}

} // namespace VoicePanel
