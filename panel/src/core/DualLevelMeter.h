#pragma once
#include <QWidget>

// One compact always-visible meter with two thin bars: microphone (top) and
// remote/call audio (bottom). It replaces the two separate QProgressBar rows
// that used to sit in the collapsed settings of the live and meeting windows:
// a level the user cannot see while recording is not a level. Levels arrive
// as raw RMS and are normalised through AudioMeter::normalized, so every
// meter in the app moves alike. Repaints only on new input, no timers.
namespace VoicePanel {

class DualLevelMeter : public QWidget {
    Q_OBJECT
public:
    explicit DualLevelMeter(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {200, 20}; }
    QSize minimumSizeHint() const override { return {120, 20}; }

public slots:
    void setMicLevel(double rms);
    void setRemoteLevel(double rms);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    static double smooth(double current, double goal);
    double mic_ = 0.0;
    double remote_ = 0.0;
};

} // namespace VoicePanel