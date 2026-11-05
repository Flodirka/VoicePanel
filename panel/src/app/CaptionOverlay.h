#pragma once
#include <QPoint>
#include <QWidget>

namespace VoicePanel {
class LevelBadge;
}

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace VoicePanel {

class PanelApp;
class SessionController;

// A small always-on-top strip that captions one application's audio.
//
// Every choice is visible: the source, the direction and whether the
// translation is spoken. A right-click menu alone hid all of that, and a
// transparent background lost the text over bright video.
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
    void buildUi();
    void refreshSources();
    void restart();
    void applySettings();

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    QComboBox *source_ = nullptr;
    QComboBox *direction_ = nullptr;
    QCheckBox *voice_ = nullptr;
    LevelBadge *micBadge_ = nullptr;
    QPushButton *close_ = nullptr;
    QLabel *heard_ = nullptr;
    QLabel *translated_ = nullptr;

    bool running_ = false;
    bool loading_ = false;
    QPoint dragOffset_;
    bool dragging_ = false;
};

} // namespace VoicePanel
