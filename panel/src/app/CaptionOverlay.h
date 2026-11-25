#pragma once
#include <QList>
#include <QPoint>
#include <QRect>
#include <QWidget>

class QSizeGrip;
class QTextCursor;
class QTextEdit;

namespace VoicePanel {
class LevelBadge;
class SettingsPopover;
}

class QComboBox;
class QPushButton;

namespace VoicePanel {

class PanelApp;
class SessionController;

// A minimal always-on-top caption strip, modelled on the mobile AI-subtitles
// designs: a compact toolbar (settings, direction, mic state, close) and a
// caption card that reads like a transcript — the heard line above the
// translation, newest at the bottom, scrollable and always following the
// speech. Every knob lives in the settings popover (audio source, what to
// show, text size), so the strip itself stays as small as the caption.
//
// The strip is frameless, so it carries its own resize borders and a size
// grip; its geometry is remembered between sessions.
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
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    // The card is painted here: a stylesheet background on a translucent
    // top-level widget is not painted, which left the text on bare screen.
    void paintEvent(QPaintEvent *event) override;

signals:
    void recordingChanged(bool recording);

private:
    struct TextSizes { int heard; int translated; };
    // One caption block in the card: the heard line, the translation, or a
    // status notice. Blocks are kept so a display or text-size change can
    // re-render the card without losing what was already said.
    struct CaptionLine {
        quint64 id = 0;          // phrase identity for a late speaker label
        QString speaker;
        QString heard;
        QString translated;
        bool notice = false;
    };

    TextSizes textSizes() const;
    void buildUi();
    void buildSettings();
    void refreshSources();
    void restart();
    void applySettings();
    void applyDisplayMode();     // what to show: both / translation / transcript
    void applyTextSize();        // S / M / L pixel sizes
    void addCaption(const QString &speaker, const QString &heard, const QString &translated,
                    quint64 id);
    void addNotice(const QString &text);
    // The diariser answers after the words are already on screen: only that
    // caption's speaker prefix is redrawn, not the whole card.
    void relabel(quint64 id, const QString &speaker);
    // Repaint one caption block in place, keeping the reader's scroll position.
    void paintLine(quint64 id, const CaptionLine &line);
    void appendBlock(const CaptionLine &line, bool follow);
    void writeRuns(QTextCursor &cursor, const CaptionLine &line);
    void renderCaptions();
    void clearCaptions();
    void showPlaceholder();
    bool following() const;
    void scrollToNewest();
    Qt::Edges edgesAt(const QPoint &pos) const;
    void updateHoverCursor(const QPoint &pos);
    void saveGeometryToSettings();

    PanelApp *app_ = nullptr;
    SessionController *controller_ = nullptr;
    QComboBox *source_ = nullptr;
    QComboBox *direction_ = nullptr;
    QComboBox *display_ = nullptr;
    QComboBox *size_ = nullptr;
    SettingsPopover *settings_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    LevelBadge *micBadge_ = nullptr;
    QPushButton *close_ = nullptr;
    QTextEdit *captions_ = nullptr;
    QSizeGrip *grip_ = nullptr;

    QList<CaptionLine> history_;
    bool placeholderShown_ = false;
    bool running_ = false;
    bool loading_ = false;
    QPoint dragOffset_;
    bool dragging_ = false;
    Qt::Edges resizeEdges_;
    QRect resizeStartGeometry_;
    QPoint resizeStartGlobal_;
};

} // namespace VoicePanel
