#pragma once
#include <QObject>
#include <QDateTime>

// Optional local Markdown records: one file per recording session, UTF-8,
// YAML front matter + original/corrected/translation blocks. Atomic writes.
namespace VoicePanel {

struct SessionRecord {
    QString sessionId;
    QString mode;
    QString sourceLang;
    QString targetLang;
    QString original;      // raw ASR transcript
    QString corrected;     // corrected/transcribed-final text (if any)
    QString translation;   // translation output (if any)
    QString finalText;     // user-confirmed text ("" until user confirms)
    QDateTime started;
    QDateTime ended;
};

class MarkdownStore : public QObject {
    Q_OBJECT
public:
    explicit MarkdownStore(QObject *parent = nullptr);

    void setDir(const QString &dir);      // empty disables storage
    QString dir() const { return dir_; }
    bool enabled() const { return !dir_.isEmpty(); }

    // Creates the session file if absent, appends a transcription record.
    // Crash-safe: writes to a temp file then atomically replaces.
    bool saveRecord(const SessionRecord &record, QString *errorOut = nullptr);

    // Deletes raw-audio files older than `days` (0 = delete none/keep disabled).
    static void enforceAudioRetention(const QString &audioDir, int days);

private:
    QString dir_;
};

} // namespace VoicePanel
