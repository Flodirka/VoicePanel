#pragma once
#include <QDialog>

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QTextEdit;

namespace VoicePanel {

class LevelBadge;
class PanelApp;
class SessionController;
class SettingsPopover;

// The live surface, two modes: the interpreter (speak both ways) and
// subtitles (listen only).
//
// The interpreter reads as the mobile translate-conversation design the user
// approved: the window splits into two equal halves — the call on the left,
// the microphone on the right — divided by a hairline with a swap button.
// Each half is headed by its animated level badge (the speaker glyph for call
// audio, the microphone for the user), and each utterance lands as a caption
// block: the heard line muted under a side-coloured marker, the translation
// below it. Settings live in a popover, not in a permanently visible form.
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
    void buildSettings();
    void refreshDevices();
    void refreshVoices();
    void applySettings();
    void startListening();
    void setRunning(bool running);
    void updateExplanation();
    void updateCableHint();
    void appendRemote(const QString &heard, const QString &translated);
    void appendMine(const QString &said, const QString &voiced);
    void clearPanes();
    QString settingsSafeName() const;

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    Mode mode_ = Mode::Interpreter;

    SettingsPopover *settings_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
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
    QPushButton *refreshButton_ = nullptr;
    QPushButton *swapButton_ = nullptr;
    LevelBadge *micBadge_ = nullptr;
    LevelBadge *remoteBadge_ = nullptr;
    QLabel *cableHint_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QFrame *sideDivider_ = nullptr;
    QWidget *interpreterBody_ = nullptr;
    QWidget *mineBox_ = nullptr;
    QTextEdit *remoteEdit_ = nullptr;     // the call half (interpreter) / captions (subtitles)
    QTextEdit *mineEdit_ = nullptr;       // the microphone half

    bool running_ = false;
    int previousOutputDevice_ = -1;
};

} // namespace VoicePanel
