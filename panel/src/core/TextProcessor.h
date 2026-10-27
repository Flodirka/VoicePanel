#pragma once
#include "core/ServiceClient.h"
#include "core/SpellingChecker.h"
#include <QObject>
#include <QRegularExpression>

// Sentence-aware correction/translation orchestration.
//
// Rules from the specification:
//  * The LLM never sees unstable fragments: the minimum unit is a complete
//    sentence cut from *stable* (finalized) transcript text.
//  * A VAD pause does not imply a sentence boundary; punctuation decides.
//  * When a recording ends mid-sentence, the fragment is preserved as-is.
//  * Late correction results never overwrite text the user has edited.
namespace VoicePanel {

class TextProcessor : public QObject {
    Q_OBJECT
public:
    explicit TextProcessor(ServiceClient *service, QObject *parent = nullptr);

    void setMode(const QString &mode) { mode_ = mode; }
    void setProfile(const QString &profile) { profile_ = profile; }
    void setAutoTranslate(bool enabled) { autoTranslate_ = enabled; }

    // Queue finalized transcript text for processing. Only complete sentences
    // are dispatched; incomplete tails wait for more text or stop().
    void acceptFinal(const QString &finalText);
    // End of session: any complete prefix is processed; the remainder is
    // emitted as-is via fragmentRemaining().
    void finish();
    void reset();

    QString pendingOriginal() const { return pendingOriginal_; }
    bool busy() const { return inFlight_; }

signals:
    // Processed text for a batch of complete sentences (original kept too).
    void processedBatch(const QString &original, const QString &result);
    // Unprocessed tail at end of session (incomplete sentence).
    void fragmentRemaining(const QString &fragment);
    void failed(const QString &error);
    void finished();
    void pendingChanged(const QString &text);

private:
    void dispatchPending();
    void maybeFinishAfterFlight();
    static QPair<QString, QString> splitCompletePrefix(const QString &text);

    ServiceClient *service_;
    QString mode_;
    QString profile_;
    QString pendingOriginal_;      // stable text not yet dispatched
    QString inFlightOriginal_;
    QString lastProcessedResult_;  // context for the next batch
    bool inFlight_ = false;
    bool autoTranslate_ = false;
    bool finishing_ = false;
    bool failed_ = false;
    quint64 generation_ = 0;
};

} // namespace VoicePanel
