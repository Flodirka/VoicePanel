#pragma once
#include <QWidget>

// Small canvas audio-activity meter for the compact bar; repaints only on new
// levels, no timers while idle.
namespace VoicePanel {

class AudioMeter : public QWidget {
    Q_OBJECT
public:
    explicit AudioMeter(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {64, 18}; }

    // The one shared RMS -> 0..1 mapping (speech RMS hovers around 0.02-0.3,
    // so it is logarithmic). Every meter in the app uses this, so the bars in
    // the panel, the live window and the meeting window move alike.
    static double normalized(double rms);

public slots:
    void setLevel(double rms);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    double level_ = 0.0;
    double smooth_ = 0.0;
};

} // namespace VoicePanel
