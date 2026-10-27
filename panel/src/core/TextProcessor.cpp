#include "core/TextProcessor.h"
#include <QPointer>

using namespace VoicePanel;

namespace {

bool hasCyrillic(const QString &text) {
    for (const QChar ch : text) {
        if (ch.unicode() >= 0x0400 && ch.unicode() <= 0x04FF) return true;
    }
    return false;
}

} // namespace

// Sentence-final punctuation not embedded in numbers/versions ("2.4.1").
static const QRegularExpression kFinalPunct(QStringLiteral("(?<!\\d)[.!?…]+(?:\\s+|$)"));

TextProcessor::TextProcessor(ServiceClient *service, QObject *parent)
    : QObject(parent), service_(service) {}

QPair<QString, QString> TextProcessor::splitCompletePrefix(const QString &text) {
    const QString stripped = text.trimmed();
    qsizetype lastEnd = -1;
    QRegularExpressionMatchIterator it = kFinalPunct.globalMatch(stripped);
    while (it.hasNext()) {
        lastEnd = it.next().capturedEnd();
    }
    if (lastEnd < 0) return {QString(), stripped};
    return {stripped.left(lastEnd).trimmed(), stripped.mid(lastEnd).trimmed()};
}

void TextProcessor::acceptFinal(const QString &finalText) {
    if (mode_ == "dictation" && !autoTranslate_) {
        emit processedBatch(finalText, finalText);
        return;
    }
    const QString add = finalText.trimmed();
    if (!add.isEmpty()) {
        pendingOriginal_ = pendingOriginal_.isEmpty() ? add : pendingOriginal_ + " " + add;
    }
    emit pendingChanged((inFlightOriginal_ + " " + pendingOriginal_).trimmed());
    dispatchPending();
}

void TextProcessor::dispatchPending() {
    if (failed_) {
        maybeFinishAfterFlight();
        return;
    }
    if (inFlight_ || (mode_ == "dictation" && !autoTranslate_)) {
        maybeFinishAfterFlight();
        return;
    }
    const auto [complete, rest] = splitCompletePrefix(pendingOriginal_);
    if (complete.isEmpty()) {
        maybeFinishAfterFlight();
        return;
    }
    pendingOriginal_ = rest;
    inFlight_ = true;
    inFlightOriginal_ = complete;
    const QString original = complete;
    const QString previous = lastProcessedResult_;
    // "auto_correct" picks the correction language from the transcript itself,
    // so users never have to choose a language mode up front.
    const QString mode = autoTranslate_ ? (hasCyrillic(original) ? "translate_ru_en" : "translate_en_ru")
                         : mode_ == "auto_correct"
                             ? (hasCyrillic(original) ? "correct_ru" : "correct_en")
                             : mode_;
    const QString profile = profile_;
    const quint64 generation = generation_;
    QPointer<TextProcessor> self(this);
    service_->processText(original, mode, profile, previous, {},
                          [this, self, generation, original](bool ok, QString result, QString notes, QString err) mutable {
        if (!self || generation != generation_) return;
        inFlight_ = false;
        inFlightOriginal_.clear();
        if (!ok) {
            failed_ = true;
            // keep the text for a later retry; never lose user content
            pendingOriginal_ = pendingOriginal_.isEmpty() ? original : original + " " + pendingOriginal_;
            emit pendingChanged(pendingOriginal_);
            emit failed(err);
            maybeFinishAfterFlight();
            return;
        }
        lastProcessedResult_ = result;
        emit pendingChanged(pendingOriginal_);
        emit processedBatch(original, result);
        if (!notes.isEmpty()) emit failed(tr("Correction notes: %1").arg(notes));  // info channel
        dispatchPending();
    });
}

void TextProcessor::finish() {
    finishing_ = true;
    dispatchPending();               // processes any remaining complete sentences
    maybeFinishAfterFlight();        // emits the incomplete tail if nothing is in flight
}

void TextProcessor::maybeFinishAfterFlight() {
    if (finishing_ && !inFlight_) {
        finishing_ = false;
        if (failed_) {
            emit finished();
            return;
        }
        const QString tail = pendingOriginal_;
        pendingOriginal_.clear();
        emit pendingChanged({});
        if (!tail.trimmed().isEmpty()) {
            // Incomplete sentence: preserved verbatim, never forced through the LLM.
            emit fragmentRemaining(tail);
        }
        emit finished();
    }
}

void TextProcessor::reset() {
    ++generation_;
    pendingOriginal_.clear();
    inFlightOriginal_.clear();
    lastProcessedResult_.clear();
    inFlight_ = false;
    finishing_ = false;
    failed_ = false;
}
