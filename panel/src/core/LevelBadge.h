#pragma once

#include <QWidget>

namespace VoicePanel {

// Animated level indicator: a Lucide-style glyph whose fill follows the live
// audio level. Replaces the raw bar meters in the live windows — the bars
// froze whenever loopback capture went quiet (WASAPI delivers no packets in
// silence, so no level events arrived and nothing repainted), and they read
// as decoration rather than state. The badge redraws itself on a short decay
// timer, so it always settles back to idle even when events stop arriving.
class LevelBadge : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Microphone, Remote };
    explicit LevelBadge(Kind kind, QWidget *parent = nullptr);

    void setLevel(double normalized);      // 0..1, already log-normalized
    void setRecording(bool recording);     // microphone state; also tints the ring
    void reset();                          // idle, not recording

    QSize sizeHint() const override { return {kBadge, kBadge}; }
    QSize minimumSizeHint() const override { return {kBadge, kBadge}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void refresh();                        // decay step, called by the timer

    Kind kind_;
    double level_ = 0.0;                   // smoothed, 0..1
    bool recording_ = false;
    QTimer *decay_;
    static constexpr int kBadge = 24;   // the icon grid (UiKit renders 16/20/24/32)
};

} // namespace VoicePanel
