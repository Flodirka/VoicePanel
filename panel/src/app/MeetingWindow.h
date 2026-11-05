#pragma once
#include <QDialog>
#include <QMap>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTextEdit;

namespace VoicePanel {

class DualLevelMeter;
class PanelApp;
class SessionController;

// Meeting transcription has its own window: it records both sources for a long
// time and produces a labelled transcript, which is a different job from the
// call interpreter's two-column, turn-by-turn view.
class MeetingWindow : public QDialog {
    Q_OBJECT
public:
    explicit MeetingWindow(PanelApp *app, QWidget *parent = nullptr);
    ~MeetingWindow() override;

    void startRecording();
    void stopRecording();

private:
    void buildUi();
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
    void renameSpeaker(const QString &speaker);
    QString settingsSafeName() const;

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    QComboBox *micBox_ = nullptr;
    QComboBox *callBox_ = nullptr;
    QComboBox *languageBox_ = nullptr;
    QCheckBox *diarize_ = nullptr;
    QPushButton *startButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QTextEdit *transcript_ = nullptr;
    DualLevelMeter *meters_ = nullptr;
    QVector<Line> lines_;
    QMap<QString, QString> names_;
    bool running_ = false;
};

} // namespace VoicePanel
