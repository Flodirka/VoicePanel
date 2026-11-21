#include "core/SessionController.h"
#include "core/LiveSession.h"
#include "core/ServiceClient.h"
#include "core/TtsPlayer.h"

#include <QDataStream>
#include <QDateTime>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

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
        // What the call hears is the user's own voice. An empty language row
        // means the saved profile; without one it stays empty and the service
        // answers with the model's own voice (Piper for Russian on a fresh
        // install).
        const QString russian = sessionVoice(settings, settings.ttsRussianVoice);
        const QString english = sessionVoice(settings, settings.ttsEnglishVoice);
        QString model = settings.ttsModel;
        const bool automatic = model.isEmpty() || model == QLatin1String("auto")
            || model == QLatin1String("profile") || model == QLatin1String("default");
        // A profile is synthesised by a cloning engine; the automatic route is
        // what resolves it. A model the user named stays untouched.
        if (automatic && (!russian.isEmpty() || !english.isEmpty())) model = QStringLiteral("auto");
        tts_->setModel(model);
        tts_->setVoiceProfile(settings.ttsVoiceProfile);
        tts_->setLanguageVoices(russian, english);
        tts_->setSpeed(settings.ttsSpeed);
        tts_->setPauseMs(settings.ttsPauseMs);
    }
}

bool SessionController::start(const QString &vadModelPath) {
    if (running_) return true;
    transcript_.clear();
    micQueue_.clear();
    remoteQueue_.clear();
    translationQueue_.clear();
    hasCurrent_ = false;
    busy_ = false;
    translating_ = false;
    // A new session is a new call: the diariser numbers people per run, and
    // leftover identity from the previous one would label the first phrase of
    // a fresh recording with an old speaker number.
    remoteTimeline_.clear();
    timelineStartAt_ = 0.0;
    timelineStarted_ = false;
    knownTurns_.clear();
    nextSpeaker_ = 1;
    pendingSpeaker_.clear();
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
    translationQueue_.clear();
    hasCurrent_ = false;
    busy_ = false;
    translating_ = false;
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
    // Remote meeting speech and captioned application audio are labelled by a
    // separate diariser, so the words can come from whichever recogniser
    // handles the language best. Both share one rolling window, so a voice
    // keeps its number for as long as it keeps speaking.
    const bool needsDiarize =
        current_.incoming && (mode_ == Mode::Meeting || mode_ == Mode::Subtitles);
    currentRecognized_ = false;
    currentDiarized_ = !needsDiarize;
    currentText_.clear();
    currentError_.clear();
    auto recognize = [this, self, generation, wav, model, language] {
        if (!self || generation != generation_) return;
        service_->transcribe(wav, model, language,
                             [this, self, generation](bool ok, QString text, QString error) {
            if (!self || generation != generation_) return;
            currentRecognized_ = true;
            if (ok) currentText_ = text.trimmed();
            else currentError_ = error;
            maybeFinishCurrent();
        });
    };
    if (needsDiarize) {
        // The diariser is another process, so it is started beside recognition.
        // Waiting for it first made every phrase cost the sum of both engines.
        startDiarize([this, self, generation] {
            if (!self || generation != generation_) return;
            currentDiarized_ = true;
            maybeFinishCurrent();
        });
    }
    recognize();
}

void SessionController::maybeFinishCurrent() {
    if (!hasCurrent_ || !currentRecognized_ || !currentDiarized_) return;
    hasCurrent_ = false;
    if (!currentError_.isEmpty()) {
        busy_ = false;
        emit error(tr("Recognition failed: %1").arg(currentError_));
        pump();
        return;
    }
    finishCurrent(currentText_);
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
                for (int t = 0; t < turns.size(); ++t) {
                    const Turn &turn = turns[t];
                    // The recogniser's [Sxx] numbers restart per recognition
                    // window; a 3-person call would surface every renumbering
                    // as a new participant. Canonical identity comes from the
                    // phrase timeline instead, like the separate diariser.
                    const double start = job.at + turn.at;
                    const double end = t + 1 < turns.size()
                        ? qMax(start + 0.5, job.at + turns[t + 1].at)
                        : start + 4.0;
                    speakers << speakerLabel(canonicalSpeaker(start, end));
                    pieces << turn.text;
                    offsets << start;
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

    // Subtitles. Recognition and translation are separate queues: releasing
    // the recogniser here is what lets the next phrase be heard while this one
    // is still being translated. One slow translation used to stall every
    // following phrase, which is what made the overlay fall behind.
    if (mode_ == Mode::Subtitles) {
        busy_ = false;
        // One label per phrase, taken from the same window the meeting uses.
        // Empty until the diariser has heard this stream's first voice.
        const QString speaker = pendingSpeaker_;
        pendingSpeaker_.clear();
        if (settings_.subtitlesDisplay == 2) {
            // Transcript only: no translator call is needed at all.
            emit caption(speaker, text, QString());
        } else {
            translationQueue_.append({speaker, text});
            pumpTranslation();
        }
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
                emit caption(QString(), text, result.trimmed());
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


void SessionController::pumpTranslation() {
    if (translating_ || translationQueue_.isEmpty() || !running_) return;
    const QPair<QString, QString> queued = translationQueue_.takeFirst();
    const QString speaker = queued.first;
    const QString heard = queued.second;
    translating_ = true;
    const QPointer<SessionController> self(this);
    const quint64 generation = generation_;
    // Same direction the interpreter uses: 0 hears English, 1 hears Russian.
    const QString intoMine = settings_.subtitlesLanguage == 1 ? QStringLiteral("translate_ru_en")
                                                             : QStringLiteral("translate_en_ru");
    service_->processText(heard, intoMine, settings_.profile, QString(), {},
                          [this, self, generation, speaker, heard](bool ok, QString result, QString, QString error) {
        if (!self || generation != generation_) return;
        translating_ = false;
        if (!ok) {
            // The queue continues with the next phrase; the failure is reported
            // once, exactly like the interpreter reports it.
            emit this->error(tr("Translation failed: %1").arg(error));
        } else {
            const QString translated = result.trimmed();
            emit caption(speaker, heard, translated);
        }
        pumpTranslation();
    });
}

QString SessionController::sessionVoice(const Settings &settings, const QString &chosen) {
    // Settings::load has already turned the retired "builtin" placeholder into
    // an empty row (normalizeVoiceSelection), so empty is the only case here.
    return chosen.isEmpty() ? settings.ttsVoiceProfile : chosen;
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

void SessionController::recordKnownTurn(int speaker, double start, double end) {
    if (!speaker || end <= start) return;
    // Merge with every span this person already has that touches this one, so
    // a person's speech is a union of intervals rather than a pile of copies.
    // The rolling window re-reports the last two minutes on every phrase; the
    // copies used to outvote the voice actually speaking.
    KnownTurn span{start, end, speaker};
    for (int i = 0; i < knownTurns_.size();) {
        const KnownTurn &known = knownTurns_[i];
        if (known.speaker == speaker && span.start <= known.end && known.start <= span.end) {
            span.start = qMin(span.start, known.start);
            span.end = qMax(span.end, known.end);
            knownTurns_.removeAt(i);
            i = 0;          // a merged span can bridge two more
            continue;
        }
        ++i;
    }
    knownTurns_.append(span);
}

int SessionController::canonicalSpeaker(double start, double end) {
    // Same person, next phrase: the most overlapping known turn wins. Offline
    // diarisation numbers speakers per call, so without this every phrase would
    // restart at "Speaker 1". A phrase usually starts after a pause, so the
    // match allows a small gap around each known turn — otherwise a speaker who
    // merely pauses between phrases would get a new number every time.
    //
    // `start`/`end` are on the ABSOLUTE session clock (the LiveSession `at` of
    // the phrase, plus per-turn offsets). The diariser's rolling buffer trims
    // its own head over time; matching on its shifted per-window times instead
    // manufactured a new speaker for every phrase once trimming started.
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
    recordKnownTurn(matched, start, end);
    while (knownTurns_.size() > 400) knownTurns_.removeFirst();
    return matched;
}

QString SessionController::speakerLabel(int canonical) {
    return canonical > 0 ? tr("Speaker %1").arg(canonical) : tr("Others");
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
    // Known-turn identity lives on the absolute session clock: the rolling
    // buffer trims its head, so per-window times drift once trimming starts.
    const double timelineStart = timelineStartAt_;
    service_->diarize(encodeWav(window),
                      [this, self, generation, phraseStart, phraseEnd, windowBase, timelineStart, then](
                          bool ok, QJsonArray turns, QString error) {
        if (!self || generation != generation_) return;
        if (ok && !turns.isEmpty()) {
            // A diariser numbers its speakers per request, so "speaker_1" only
            // means something inside one window. Each of the window's ids is
            // therefore resolved against the speech already labelled, and ALL
            // of its turns are recorded under the resolved person — not just
            // the phrase's own one. A meeting leaves long pauses between
            // phrases; remembering one short span per phrase left most of the
            // next window unlabelled, so a phrase that fell between two known
            // spans minted a fresh speaker every time the window advanced.
            struct TurnSpan {
                double start;
                double end;
                QString id;
            };
            QVector<TurnSpan> spans;
            QMap<QString, double> inPhrase;           // id -> seconds inside the phrase
            QMap<QString, double> totals;             // id -> seconds in the window
            for (const QJsonValue &value : turns) {
                const QJsonObject turn = value.toObject();
                const double start = windowBase
                    + turn.value(QStringLiteral("start_sample")).toDouble() / 16000.0;
                const double end = windowBase
                    + turn.value(QStringLiteral("end_sample")).toDouble() / 16000.0;
                const QString id = turn.value(QStringLiteral("speaker_id")).toString();
                if (id.isEmpty() || end <= start) continue;
                // Absolute session clock: known turns are stored there and
                // the rolling window trims its head as it advances.
                spans.append({start + timelineStart, end + timelineStart, id});
                totals[id] += end - start;
                const double overlap = qMin(end, phraseEnd) - qMax(start, phraseStart);
                if (overlap > 0.0) inPhrase[id] += overlap;
            }
            QString dominantId;
            double longest = 0.0;
            for (auto it = inPhrase.constBegin(); it != inPhrase.constEnd(); ++it) {
                if (it.value() > longest) {
                    longest = it.value();
                    dominantId = it.key();
                }
            }
            // How much of each id's own speech each already-known person
            // explains. Known spans are merged per person, so these seconds
            // are counted once: an id belongs to the person who covers the
            // larger share of it, not to whoever was recorded most often.
            QMap<QString, QMap<int, double>> covered;
            for (const TurnSpan &span : spans) {
                for (const KnownTurn &known : knownTurns_) {
                    const double overlap = qMin(span.end, known.end) - qMax(span.start, known.start);
                    if (overlap > 0.0) covered[span.id][known.speaker] += overlap;
                }
            }
            struct Candidate {
                double share;
                QString id;
                int person;
            };
            QVector<Candidate> candidates;
            for (auto id = covered.constBegin(); id != covered.constEnd(); ++id) {
                int best = 0;
                double most = 0.0;
                for (auto person = id.value().constBegin(); person != id.value().constEnd(); ++person) {
                    if (person.value() > most) {
                        most = person.value();
                        best = person.key();
                    }
                }
                const double total = totals.value(id.key());
                const double share = total > 0.0 ? qMin(1.0, most / total) : 0.0;
                if (best && share >= kDiarizeIdShare) candidates.append({share, id.key(), best});
            }
            // The phrase's own voice claims its person first: in a long window
            // the diariser sometimes splits one voice into several ids, and
            // letting another fragment take the person left the voice actually
            // speaking this phrase without an identity.
            std::sort(candidates.begin(), candidates.end(),
                      [&dominantId](const Candidate &a, const Candidate &b) {
                          const bool aHasPhrase = a.id == dominantId;
                          const bool bHasPhrase = b.id == dominantId;
                          if (aHasPhrase != bHasPhrase) return aHasPhrase;
                          return a.share > b.share;
                      });
            QMap<QString, int> resolved;
            QSet<int> taken;
            for (const Candidate &candidate : candidates) {
                if (taken.contains(candidate.person)) continue;
                resolved[candidate.id] = candidate.person;
                taken.insert(candidate.person);
            }
            if (!dominantId.isEmpty()) {
                // The phrase names the person its own voice resolved to; only a
                // voice that overlaps nobody already known is a new one, and
                // only when the window really holds that voice. A fragment at
                // a turn boundary used to mint a speaker of its own, which
                // pushed the third real voice onto a fourth label.
                int canonical = resolved.value(dominantId, 0);
                if (!canonical) {
                    int nearest = 0;
                    double nearestShare = 0.0;
                    for (auto person = covered.value(dominantId).constBegin();
                         person != covered.value(dominantId).constEnd(); ++person) {
                        const double total = totals.value(dominantId);
                        const double share = total > 0.0 ? person.value() / total : 0.0;
                        if (share > nearestShare) {
                            nearestShare = share;
                            nearest = person.key();
                        }
                    }
                    const bool substantial =
                        totals.value(dominantId) >= kDiarizeNewSpeakerSeconds
                        && inPhrase.value(dominantId) >= kDiarizeNewSpeakerPhraseSeconds;
                    if (substantial || knownTurns_.isEmpty()) canonical = nextSpeaker_++;
                    // Not substantial enough to be a participant: name the
                    // phrase by the closest known voice rather than invent one.
                    else if (nearest && nearestShare >= kDiarizeNearShare) canonical = nearest;
                }
                if (canonical) {
                    resolved[dominantId] = canonical;
                    pendingSpeaker_ = speakerLabel(canonical);
                }
            }
            for (const TurnSpan &span : spans) {
                const int speaker = resolved.value(span.id, 0);
                if (!speaker) continue;
                recordKnownTurn(speaker, span.start, span.end);
            }
            while (knownTurns_.size() > 800) knownTurns_.removeFirst();
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
