#pragma once
#include <QPoint>
#include <QWidget>

namespace VoicePanel {
class LevelBadge;
class SettingsPopover;
}

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;

namespace VoicePanel {

class PanelApp;
class SessionController;

// A minimal always-on-top caption strip, modelled on the mobile AI-subtitles
// designs: a compact toolbar (settings, direction, mic state, close) and a
// caption card — the heard line above a hairline, the translation below.
// Every knob lives in the settings popover (audio source, what to show,
// text size, voice), so the strip itself stays as small as the caption.
class CaptionOverlay : public QWidget {
    Q_OBJECT
public:
    explicit CaptionOverlay(PanelApp *app, QWidget *parent = nullptr);
    ~CaptionOverlay() override;

    void begin();
    void end();
    bool active() const { return running_; }

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    // The card is painted here: a stylesheet background on a translucent
    // top-level widget is not painted, which left the text on bare screen.
    void paintEvent(QPaintEvent *event) override;

signals:
    void recordingChanged(bool recording);

private:
    struct TextSizes { int heard; int translated; };
    TextSizes textSizes() const;
    void buildUi();
    void buildSettings();
    void refreshSources();
    void restart();
    void applySettings();
    void applyDisplayMode();     // what to show: both / translation / transcript
    void applyTextSize();        // S / M / L pixel sizes

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    QComboBox *source_ = nullptr;
    QComboBox *direction_ = nullptr;
    QComboBox *display_ = nullptr;
    QComboBox *size_ = nullptr;
    QCheckBox *voice_ = nullptr;
    SettingsPopover *settings_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    LevelBadge *micBadge_ = nullptr;
    QPushButton *close_ = nullptr;
    QFrame *divider_ = nullptr;
    QLabel *heard_ = nullptr;
    QLabel *translated_ = nullptr;

    bool running_ = false;
    bool loading_ = false;
    QPoint dragOffset_;
    bool dragging_ = false;
};

} // namespace VoicePanel
