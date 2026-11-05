#pragma once
#include <QDialog>

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QSplitter;
class QTextEdit;

namespace VoicePanel {

class LevelBadge;
class PanelApp;
class SessionController;

// The live surface. It borrows the preview editor's layout — one frameless
// "editorPage" frame, muted column captions, plain-text panes and a bottom
// action row — so the two windows read as one application.
//
// Settings are collapsed by default: an interpreter wants to see the two
// columns, not eight combos.
class SessionWindow : public QDialog {
    Q_OBJECT
public:
    // Meetings have their own window; this one holds the two call modes:
    // the interpreter (speak both ways) and subtitles (listen only).
    enum class Mode { Interpreter, Subtitles };

    explicit SessionWindow(PanelApp *app, QWidget *parent = nullptr);
    ~SessionWindow() override;

    void openMode(Mode mode);
    void stopSession();

signals:
    void recordingChanged(bool recording);

private:
    void buildUi();
    void refreshDevices();
    void refreshVoices();
    void applySettings();
    void startListening();
    void setRunning(bool running);
    void updateExplanation();
    void updateCableHint();
    void appendHeard(const QString &text);
    void appendTranslated(const QString &text);
    void appendLine(const QString &text);
    QString settingsSafeName() const;

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    Mode mode_ = Mode::Interpreter;

    QPushButton *settingsToggle_ = nullptr;
    QWidget *settingsBox_ = nullptr;
    QPushButton *tabInterpreter_ = nullptr;
    QPushButton *tabMeeting_ = nullptr;
    QComboBox *micBox_ = nullptr;
    QComboBox *callBox_ = nullptr;
    QComboBox *speakBox_ = nullptr;
    QComboBox *voiceBox_ = nullptr;
    QComboBox *outgoingBox_ = nullptr;
    QComboBox *callLanguageBox_ = nullptr;
    QPushButton *previewButton_ = nullptr;
    QPushButton *startButton_ = nullptr;
    QPushButton *closeButton_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    LevelBadge *micBadge_ = nullptr;
    LevelBadge *remoteBadge_ = nullptr;
    QFormLayout *settingsForm_ = nullptr;
    QLabel *cableHint_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *titleLabel_ = nullptr;
    QLabel *heardLabel_ = nullptr;
    QLabel *translatedLabel_ = nullptr;
    QSplitter *columns_ = nullptr;
    QWidget *translatedPane_ = nullptr;
    QTextEdit *heardEdit_ = nullptr;
    QTextEdit *translatedEdit_ = nullptr;

    bool running_ = false;
    int previousOutputDevice_ = -1;
};

} // namespace VoicePanel
