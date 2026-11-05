#pragma once
#include <QDateTime>
#include <QDialog>
#include <QMap>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDateTime;
class QLabel;
class QPushButton;
class QTextEdit;

namespace VoicePanel {

class LevelBadge;
class PanelApp;
class SessionController;
class SettingsPopover;

// Meeting transcription: two tabs in one window — free-form Notes and the
// live speaker-labelled Transcript — with the controls reduced to refresh and
// a settings popover (devices, language, speaker separation, and the
// auto-save folder). On stop the meeting lands in one Markdown file named
// after its full start time, Notes section first, Transcript second.
class MeetingWindow : public QDialog {
    Q_OBJECT
public:
    explicit MeetingWindow(PanelApp *app, QWidget *parent = nullptr);
    ~MeetingWindow() override;

    void startRecording();
    void stopRecording();

signals:
    void recordingChanged(bool recording);

private:
    void buildUi();
    void buildSettings();
    void refreshDevices();
    void applySettings();
    void setRunning(bool running);
    // Speaker names are a session-level label map: rename "Speaker 1" once and
    // every line, including the saved transcript, uses the new name.
    struct Line {
        QString speaker;
        QString text;
        double at = 0.0;
    };
    void appendLine(const QString &speaker, const QString &text, double at);
    void rebuildTranscript();
    QString transcriptText() const;
    QString documentText() const;          // Notes section first, Transcript second
    QString defaultFolder() const;
    void renameSpeaker(const QString &speaker);

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    SettingsPopover *settings_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QComboBox *micBox_ = nullptr;
    QComboBox *callBox_ = nullptr;
    QComboBox *languageBox_ = nullptr;
    QCheckBox *diarize_ = nullptr;
    QCheckBox *autoSave_ = nullptr;
    QLabel *folderLabel_ = nullptr;
    QPushButton *tabNotes_ = nullptr;
    QPushButton *tabTranscript_ = nullptr;
    QPushButton *startButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QTextEdit *notes_ = nullptr;
    QTextEdit *transcript_ = nullptr;
    LevelBadge *micBadge_ = nullptr;
    LevelBadge *remoteBadge_ = nullptr;
    QVector<Line> lines_;
    QMap<QString, QString> names_;
    QDateTime sessionStartedAt_;
    bool running_ = false;
};

} // namespace VoicePanel
