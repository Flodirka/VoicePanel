#include "core/SessionController.h"
#include "core/LiveSession.h"
#include "core/ServiceClient.h"
#include "core/TimingLog.h"
#include "core/TtsPlayer.h"

#include <QDataStream>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

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
    resolvedSpeakers_.clear();
    shownSpeakers_.clear();
    captionSpans_.clear();
    lastSpeakerLabel_.clear();
    diarizing_ = false;
    coveredEnd_ = 0.0;
    nextPhraseId_ = 0;
    mergeAudio_.clear();
    mergeAt_ = 0.0;
    mergedPhrases_ = 0;
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
    resolvedSpeakers_.clear();
    shownSpeakers_.clear();
    captionSpans_.clear();
    lastSpeakerLabel_.clear();
    diarizing_ = false;
    coveredEnd_ = 0.0;
    mergeAudio_.clear();
    mergeAt_ = 0.0;
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
    if (incoming && mode_ == Mode::Subtitles && !mergeAudio_.isEmpty()) {
        // The held fragment and this phrase are one utterance for the
        // recogniser; the caption belongs to this phrase and starts at the
        // fragment, so its words and its label cover the merged span.
        QVector<float> combined = mergeAudio_;
        const double expected = mergeAt_ + double(combined.size()) / 16000.0;
        const int gap = qMax(0, int((at - expected) * 16000.0));
        combined.resize(combined.size() + gap, 0.0f);
        combined += samples;
        const double start = mergeAt_;
        mergeAudio_.clear();
        mergeAt_ = 0.0;
        remoteQueue_.append(Job{combined, true, start, ++nextPhraseId_});
        pump();
        return;
    }
    (incoming ? remoteQueue_ : micQueue_).append(Job{samples, incoming, at, ++nextPhraseId_});
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
        const quint64 phraseId = current_.id;
        startDiarize([this, self, generation] {
            if (!self || generation != generation_) return;
            currentDiarized_ = true;
            maybeFinishCurrent();
        }, phraseId);
    }
    recognize();
}

void SessionController::maybeFinishCurrent() {
    if (!hasCurrent_ || !currentRecognized_) return;
    // Subtitles do not wait for the speaker label. The rolling diarisation
    // window covers two minutes and can take seconds to answer; gating the
    // caption on it made the overlay lag the interpreter by exactly that long.
    // The resolved label arrives later as a correction for the same phrase.
    if (mode_ != Mode::Subtitles && !currentDiarized_) return;
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
        // T-1 merge: the phrase is the tail of a word the previous caption
        // already carries, or the start of the next reply. Hold its audio and
        // recognise it with the next phrase instead of giving it a line.
        if (mode_ == Mode::Subtitles && job.incoming && !job.samples.isEmpty()) {
            if (mergeAudio_.isEmpty()) mergeAt_ = job.at;
            const double expected = mergeAt_ + double(mergeAudio_.size()) / 16000.0;
            const int gap = qMax(0, int((job.at - expected) * 16000.0));
            const int total = mergeAudio_.size() + gap + job.samples.size();
            if (double(total) / 16000.0 <= 3.0) {
                mergeAudio_.resize(mergeAudio_.size() + gap, 0.0f);
                mergeAudio_ += job.samples;
                ++mergedPhrases_;
            }
            // The phrase was put on the timeline when it was submitted. It is
            // held now, so the timeline ends where it begins and the merged
            // phrase appends there instead of duplicating the fragment.
            if (remoteTimeline_.size() >= job.samples.size())
                remoteTimeline_.resize(remoteTimeline_.size() - job.samples.size());
            coveredEnd_ = qMin(coveredEnd_,
                               timelineStartAt_ + double(remoteTimeline_.size()) / 16000.0);
        }
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
                // This recogniser tags no speaker turns, so the phrase keeps
                // the label the diariser last resolved: a person usually keeps
                // talking, and a missed change is corrected on the next phrase.
                speakers << (lastSpeakerLabel_.isEmpty() ? tr("Others") : lastSpeakerLabel_);
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
        // The label of this phrase if the diariser already answered, otherwise
        // the last one it resolved: a speaker usually keeps talking, and a
        // missed change is corrected on the line itself moments later.
        const QString speaker = resolvedSpeakers_.value(job.id, lastSpeakerLabel_);
        captionSpans_.insert(job.id, {job.at, job.at + double(job.samples.size()) / 16000.0});
        for (auto it = captionSpans_.begin(); it != captionSpans_.end();) {
            if (it.value().end < job.at - kCaptionMemory) it = captionSpans_.erase(it);
            else ++it;
        }
        if (settings_.subtitlesDisplay == 2) {
            // Transcript only: no translator call is needed at all.
            emitCaption(job.id, speaker, text, QString());
        } else {
            translationQueue_.append({job.id, speaker, text});
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
    const TranslationJob queued = translationQueue_.takeFirst();
    const QString heard = queued.heard;
    translating_ = true;
    const QPointer<SessionController> self(this);
    const quint64 generation = generation_;
    // Same direction the interpreter uses: 0 hears English, 1 hears Russian.
    const QString intoMine = settings_.subtitlesLanguage == 1 ? QStringLiteral("translate_ru_en")
                                                             : QStringLiteral("translate_en_ru");
    service_->processText(heard, intoMine, settings_.profile, QString(), {},
                          [this, self, generation, queued, heard](bool ok, QString result, QString, QString error) {
        if (!self || generation != generation_) return;
        translating_ = false;
        if (!ok) {
            // The queue continues with the next phrase; the failure is reported
            // once, exactly like the interpreter reports it.
            emit this->error(tr("Translation failed: %1").arg(error));
        } else {
            // The diariser may have caught up while this phrase was translated.
            const QString speaker = resolvedSpeakers_.value(queued.id, queued.speaker);
            emitCaption(queued.id, speaker, heard, result.trimmed());
        }
        pumpTranslation();
    });
}

void SessionController::emitCaption(quint64 id, const QString &speaker, const QString &heard,
                                    const QString &translated) {
    shownSpeakers_.insert(id, speaker);
    emit caption(speaker, heard, translated, id);
}

void SessionController::deliverSpeaker(quint64 id, const QString &speaker) {
    if (speaker.isEmpty()) return;
    resolvedSpeakers_.insert(id, speaker);
    lastSpeakerLabel_ = speaker;
    const auto shown = shownSpeakers_.constFind(id);
    // Only a caption already on screen needs repainting, and only when the
    // diariser disagrees with the label it was drawn with.
    if (shown != shownSpeakers_.constEnd() && shown.value() != speaker) {
        shownSpeakers_[id] = speaker;
        emit captionSpeaker(id, speaker);
    }
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

void SessionController::startDiarize(std::function<void()> then, quint64 phraseId) {
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
        // The clock starts with the first phrase; nothing before it exists, so
        // the first window may not try to reach back into that silence.
        coveredEnd_ = at;
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
    // A diarisation takes seconds; issuing one per phrase built a backlog that
    // kept labels increasingly stale and took CPU from recognition. The next
    // window overlaps this phrase, so skipping it only defers the label.
    if (mode_ == Mode::Subtitles && diarizing_) {
        then();
        return;
    }
    // The window spans and the captions are both measured on the absolute
    // session clock (`timelineStartAt_` moves when the rolling buffer trims its
    // head), so the phrase keeps that clock too.
    const double phraseStart = at;
    const double phraseEnd = phraseStart + double(samples.size()) / 16000.0;
    // When the engine is behind the speech, the next window continues from the
    // audio that is already labelled (still overlapping it by half a window)
    // rather than from this phrase. The phrase is named when the drain reaches
    // it, and its caption is repainted then; jumping ahead left the speech in
    // between out of every window, so the voices there lost their labels.
    const double heard = timelineStartAt_ + double(remoteTimeline_.size()) / 16000.0;
    const double windowEnd = heard - coveredEnd_ >= kDiarizeCatchUpSeconds
        ? qMin(heard, coveredEnd_ + kDiarizeStrideSeconds) : phraseEnd;
    issueDiarize(std::move(then), phraseId, phraseStart, phraseEnd, windowEnd);
}

void SessionController::issueDiarize(std::function<void()> then, quint64 phraseId,
                                     double phraseStart, double phraseEnd, double windowEnd) {
    diarizing_ = true;
    // The window ends where the caller asked, measured on the session clock:
    // the rolling buffer's own end is the newest audio, which is ahead of the
    // audio that still has to be labelled.
    const double first = timelineStartAt_;
    const double last = first + double(remoteTimeline_.size()) / 16000.0;
    // The buffer keeps three windows. If the engine fell further behind than
    // that, the oldest audio is gone and no window can hear it; resume from
    // the buffer head rather than asking for empty windows forever.
    coveredEnd_ = qMax(coveredEnd_, first);
    const double end = qBound(first, windowEnd, last);
    const int endSample = qMin(remoteTimeline_.size(), int((end - first) * 16000.0));
    const int windowStart = qMax(0, endSample - kDiarizeWindow);
    const QVector<float> window = remoteTimeline_.mid(windowStart, endSample - windowStart);
    const double windowBase = first + double(windowStart) / 16000.0;
    const double windowSeconds = double(window.size()) / 16000.0;
    const QPointer<SessionController> self(this);
    const quint64 generation = generation_;
    // Known-turn identity lives on the absolute session clock: the rolling
    // buffer trims its head, so per-window times drift once trimming starts.
    service_->diarize(encodeWav(window),
                      [this, self, generation, phraseStart, phraseEnd, windowBase,
                       windowSeconds, phraseId, then](
                          bool ok, QJsonArray turns, QString error) {
        if (!self || generation != generation_) return;
        // A stale session's answer must not clear the flag of the request the
        // new session has in flight.
        diarizing_ = false;
        // The window answered for the audio it carried, silence included: that
        // stops the catch-up below from asking for the same seconds again.
        if (ok) coveredEnd_ = qMax(coveredEnd_, windowBase + windowSeconds);
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
                spans.append({start, end, id});
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
            const double windowFrom = windowBase;
            const double windowTo = windowFrom + windowSeconds;
            // An id is only taken to be a person when the two choose each
            // other: that person explains most of the id, and the id carries
            // most of what is known of that person here. One-sided matches are
            // what swapped two people in a window where the engine's ids were
            // less pure: a voice's id overlapped a neighbour's turn by two
            // seconds, took that neighbour's name, and the neighbour was left
            // with the other's. Whatever a one-sided match would have said is
            // settled afterwards by elimination, which needs no seconds.
            QMap<QString, int> bestPersonFor;
            QMap<int, QString> bestIdFor;
            QMap<int, double> bestIdSeconds;
            for (auto id = covered.constBegin(); id != covered.constEnd(); ++id) {
                int best = 0;
                double most = 0.0;
                for (auto person = id.value().constBegin(); person != id.value().constEnd(); ++person) {
                    if (person.value() > most) {
                        most = person.value();
                        best = person.key();
                    }
                }
                if (!best || most < kDiarizeKnownSeconds) continue;
                bestPersonFor.insert(id.key(), best);
                if (most > bestIdSeconds.value(best)) {
                    bestIdSeconds.insert(best, most);
                    bestIdFor.insert(best, id.key());
                }
            }
            // The engine sometimes merges two voices into one id. Its ids stop
            // being voices then, and nothing from such a window may be used:
            // painting from it renames lines that were already right and mints
            // numbers for people who already have one. Two substantial ids
            // choosing the same person is that failure made visible.
            bool mergedWindow = false;
            QMap<int, int> claims;
            for (auto id = bestPersonFor.constBegin(); id != bestPersonFor.constEnd(); ++id) {
                if (totals.value(id.key()) < kDiarizeNewVoiceSeconds) continue;
                if (++claims[id.value()] > 1) mergedWindow = true;
            }
            // A window that overlaps no speech already labelled carries no
            // identity evidence: its ids can be the known people in any order.
            // Minting from it is what produced Speaker 4 and Speaker 5 for
            // voices that already had numbers after the window schedule left
            // a gap. Leave the captions as they are; a window that does overlap
            // names the lines it covers.
            const bool anchored = !bestPersonFor.isEmpty();
            // Every person appears at most once (bestIdFor keeps one id per
            // person), so the mutual matches need no ordering or conflict
            // check: an id that did not win its person is left to elimination.
            QMap<QString, int> resolved;
            QSet<int> taken;
            if (!mergedWindow) {
                for (auto id = bestPersonFor.constBegin(); id != bestPersonFor.constEnd(); ++id) {
                    if (bestIdFor.value(id.value()) != id.key() && bestIdFor.contains(id.value())) continue;
                    resolved[id.key()] = id.value();
                    taken.insert(id.value());
                }
            }
            // One window hears the same room the captions already name, and
            // its ids are voices: two ids are never one person. Whatever the
            // time overlap missed can therefore be settled by elimination.
            // A quiet voice that said nothing inside the overlap with the
            // previous window used to look new and was given another number,
            // so the same person appeared as Speaker 1 and later as Speaker 4
            // and every caption after the break carried the new one.
            QMap<QString, double> unclaimed;          // id -> seconds in the window
            for (auto it = totals.constBegin(); it != totals.constEnd(); ++it) {
                if (!resolved.contains(it.key()) && it.value() >= kDiarizeKnownSeconds)
                    unclaimed.insert(it.key(), it.value());
            }
            // Everybody the captions already name, not only those heard inside
            // this window: a voice that has been quiet for a minute is still a
            // participant, and it is exactly the one whose own turns are the
            // only thing that could have matched it.
            QSet<int> freePersons;
            for (const KnownTurn &known : knownTurns_) {
                if (!taken.contains(known.speaker)) freePersons.insert(known.speaker);
            }
            if (mergedWindow) {
                // Nothing here can be trusted, so the window leaves every
                // caption as it is and records nothing: the next clean window
                // names the lines it covers.
                freePersons.clear();
                unclaimed.clear();
            }
            // First by what little overlap there is: the clearest pair wins,
            // and it cannot be used twice.
            while (!freePersons.isEmpty() && !unclaimed.isEmpty()) {
                QString bestId;
                int bestPerson = 0;
                double bestOverlap = 0.0;
                for (auto id = unclaimed.constBegin(); id != unclaimed.constEnd(); ++id) {
                    const QMap<int, double> person = covered.value(id.key());
                    for (auto it = person.constBegin(); it != person.constEnd(); ++it) {
                        if (!freePersons.contains(it.key())) continue;
                        if (it.value() > bestOverlap) {
                            bestOverlap = it.value();
                            bestId = id.key();
                            bestPerson = it.key();
                        }
                    }
                }
                if (!bestPerson) break;
                resolved[bestId] = bestPerson;
                taken.insert(bestPerson);
                freePersons.remove(bestPerson);
                unclaimed.remove(bestId);
            }
            // Then by arithmetic: one voice left and one person left means that
            // voice is that person, overlap or no overlap.
            if (freePersons.size() == 1 && unclaimed.size() == 1) {
                const int person = *freePersons.constBegin();
                const QString lastId = unclaimed.constBegin().key();
                resolved[lastId] = person;
                taken.insert(person);
                freePersons.clear();
                unclaimed.clear();
            }
            // How much of this phrase's speech the id carrying it explains. A
            // voice that holds most of the phrase is the voice the caption
            // belongs to; a sliver at a turn boundary is not.
            double phraseSpeech = 0.0;
            for (auto it = inPhrase.constBegin(); it != inPhrase.constEnd(); ++it)
                phraseSpeech += it.value();
            const double ownPhrase = inPhrase.value(dominantId);
            const bool carriesPhrase = ownPhrase >= kDiarizePhraseSeconds
                && ownPhrase * 2.0 >= phraseSpeech;
            // One person left over in a window where this voice is unknown: the
            // rest of the room is accounted for, so the phrase was that person
            // speaking after a quiet stretch. Time overlap cannot show it, the
            // voices in the window can.
            if (!dominantId.isEmpty() && !resolved.contains(dominantId)
                && freePersons.size() == 1) {
                const int person = *freePersons.constBegin();
                resolved[dominantId] = person;
                taken.insert(person);
                freePersons.clear();
                unclaimed.remove(dominantId);
            }
            bool minted = false;
            // A merged window is the one case where the ids themselves are not
            // voices: the engine cut one voice in two (or joined two). Naming
            // the phrase from it mints a number for a person who already has
            // one, which is what the single-voice acceptance caught.
            if (!dominantId.isEmpty() && !mergedWindow) {
                // The phrase names the person its own voice resolved to; only a
                // voice that overlaps nobody already known is a new one.
                int canonical = resolved.value(dominantId, 0);
                if (!canonical) {
                    // An id carrying most of a so-far unknown person's speech
                    // is that person under another number; a person who is
                    // already labelled keeps that label, and nobody new is
                    // minted from a window that does not overlap known speech.
                    if ((anchored || knownTurns_.isEmpty())
                        && (carriesPhrase || knownTurns_.isEmpty())) {
                        // Nobody labelled speaks in this id, and it is the
                        // voice this phrase was heard in: a participant the
                        // captions have not named yet. Holding the number back
                        // until the id grew past several seconds of window left
                        // a whole conversation on the previous label, because
                        // the caption falls back to the name it already showed.
                        canonical = nextSpeaker_++;
                        minted = true;
                    }
                }
                if (canonical) {
                    resolved[dominantId] = canonical;
                    if (phraseId) deliverSpeaker(phraseId, speakerLabel(canonical));
                }
            }
            // A voice nobody labelled that overlaps nobody known is a
            // participant the captions have not named, even when this phrase
            // was not spoken in it. Naming it here is what lets its lines be
            // repainted: the quiet voice in a three-way conversation rarely
            // owns the phrase that triggers a window, so waiting for that left
            // its captions wearing the previous speaker's name. One new number
            // per window at most, so a window that cuts one voice in two can
            // never double the labels in one step.
            if (!minted && !mergedWindow && (anchored || knownTurns_.isEmpty())) {
                QString unknownId;
                double unknownTime = 0.0;
                for (auto it = unclaimed.constBegin(); it != unclaimed.constEnd(); ++it) {
                    if (resolved.contains(it.key())) continue;
                    if (it.value() > unknownTime) {
                        unknownTime = it.value();
                        unknownId = it.key();
                    }
                }
                // An id that overlapped somebody known is not new, however
                // badly it matched: it is a voice this window cut badly, and a
                // number for it would be a person who does not exist.
                if (!unknownId.isEmpty() && unknownTime >= kDiarizeNewVoiceSeconds
                    && !bestPersonFor.contains(unknownId))
                    resolved[unknownId] = nextSpeaker_++;
            }
            if (TimingLog::enabled()) {
                QJsonArray spanJson, knownJson;
                for (const TurnSpan &span : spans) {
                    QJsonObject one;
                    one.insert(QStringLiteral("id"), span.id);
                    one.insert(QStringLiteral("start"), span.start);
                    one.insert(QStringLiteral("end"), span.end);
                    spanJson.append(one);
                }
                for (const KnownTurn &known : knownTurns_) {
                    QJsonObject one;
                    one.insert(QStringLiteral("speaker"), known.speaker);
                    one.insert(QStringLiteral("start"), known.start);
                    one.insert(QStringLiteral("end"), known.end);
                    knownJson.append(one);
                }
                QJsonObject mappedJson;
                for (auto it = resolved.constBegin(); it != resolved.constEnd(); ++it)
                    mappedJson.insert(it.key(), it.value());
                QJsonObject dump;
                dump.insert(QStringLiteral("phrase"), double(phraseId));
                dump.insert(QStringLiteral("dominant"), dominantId);
                dump.insert(QStringLiteral("window_from"), windowFrom);
                dump.insert(QStringLiteral("window_to"), windowTo);
                dump.insert(QStringLiteral("spans"), spanJson);
                dump.insert(QStringLiteral("known"), knownJson);
                dump.insert(QStringLiteral("mapped"), mappedJson);
                TimingLog::event("o1_window", dump);
            }
            for (const TurnSpan &span : spans) {
                const int speaker = mergedWindow ? 0 : resolved.value(span.id, 0);
                if (!speaker) continue;
                recordKnownTurn(speaker, span.start, span.end);
            }
            while (knownTurns_.size() > 800) knownTurns_.removeFirst();
            // The window covers the last two minutes, so it dates every caption
            // still on screen, not only the phrase that asked for it. A label
            // that arrives after the words is a repaint: the overlay shows one
            // line, and a line keeps the name of the person the window says was
            // speaking while it was heard.
            for (auto caption = captionSpans_.constBegin();
                 caption != captionSpans_.constEnd(); ++caption) {
                // Which voice explains most of the line, counting every turn
                // it covers rather than the longest single one: the diariser
                // cuts speech into pieces of a few tenths of a second, so a
                // one-and-a-half second caption can cover half a dozen turns
                // of the same person and still miss a one-second span. That
                // left most short lines with the label they got live.
                QMap<int, double> byPerson;
                for (const TurnSpan &span : spans) {
                    const int candidate = resolved.value(span.id, 0);
                    if (!candidate) continue;
                    const double overlap = qMin(span.end, caption.value().end)
                                         - qMax(span.start, caption.value().start);
                    if (overlap > 0.0) byPerson[candidate] += overlap;
                }
                int person = 0;
                double best = 0.0;
                for (auto it = byPerson.constBegin(); it != byPerson.constEnd(); ++it) {
                    if (it.value() > best) {
                        best = it.value();
                        person = it.key();
                    }
                }
                // A voice that holds the line, or a second of it, names it.
                // Half a line is enough; a sliver at a turn boundary is not.
                double coveredTime = 0.0;
                for (auto it = byPerson.constBegin(); it != byPerson.constEnd(); ++it)
                    coveredTime += it.value();
                const double length = caption.value().end - caption.value().start;
                if (person && best * 2.0 >= coveredTime
                    && (best >= kDiarizePhraseSeconds || best * 2.0 >= length))
                    deliverSpeaker(caption.key(), speakerLabel(person));
            }
        } else if (!ok && !error.isEmpty()) {
            emit this->error(tr("Speaker separation failed: %1").arg(error));
        }
        // A window that ends far behind the audio leaves the newer lines with
        // whatever name the previous speaker had, and the phrase that would
        // normally ask for the next window may never come — the session can
        // end first. Issue it here instead; each window covers kDiarizeWindow,
        // so the gap closes and this stops by itself.
        if (ok && running_ && mode_ == Mode::Subtitles && settings_.meetingDiarize && service_
            && !diarizing_) {
            const double heard = timelineStartAt_ + double(remoteTimeline_.size()) / 16000.0;
            if (heard - coveredEnd_ >= kDiarizeCatchUpSeconds) {
                const double next = qMin(heard, coveredEnd_ + kDiarizeStrideSeconds);
                issueDiarize([] {}, 0, next, next, next);
            }
        }
        then();
    });
}

void SessionController::appendTranscript(const QString &speaker, const QString &text, double at) {
    const QString line = QStringLiteral("[%1] %2: %3\n").arg(clock(at), speaker, text);
    transcript_ += line;
}

} // namespace VoicePanel
