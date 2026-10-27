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

public slots:
    void setLevel(double rms);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    double level_ = 0.0;
    double smooth_ = 0.0;
};

} // namespace VoicePanel
