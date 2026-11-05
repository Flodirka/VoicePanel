#include "core/DualLevelMeter.h"
#include "core/AudioMeter.h"

#include <QPainter>

using namespace VoicePanel;

DualLevelMeter::DualLevelMeter(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setToolTip(tr("Microphone (top) and call audio (bottom) level"));
}

double DualLevelMeter::smooth(double current, double goal) {
    // Fast attack, slow decay: a voice onset should be visible at once, and a
    // pause should fade instead of cutting off.
    const double rate = goal > current ? 0.6 : 0.25;
    return current + (goal - current) * rate;
}

void DualLevelMeter::setMicLevel(double rms) {
    mic_ = smooth(mic_, AudioMeter::normalized(rms));
    update();
}

void DualLevelMeter::setRemoteLevel(double rms) {
    remote_ = smooth(remote_, AudioMeter::normalized(rms));
    update();
}

void DualLevelMeter::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const int w = width();
    const int barH = 5;
    const int gap = 4;
    const int top = 2;
    // Groove: both bars paint their track even at zero, so a silent meter is
    // a visible control, not an empty label.
    p.setBrush(QColor(255, 255, 255, 26));
    p.drawRoundedRect(QRectF(0.5, top + 0.5, w - 1, barH), 2.5, 2.5);
    p.drawRoundedRect(QRectF(0.5, top + barH + gap + 0.5, w - 1, barH), 2.5, 2.5);
    const auto fill = [&](double level, int y) {
        const int fw = int((w - 2) * qBound(0.0, level, 1.0));
        if (fw <= 0) return;
        const QColor color = level > 0.66 ? QColor("#e5544e")
                           : level > 0.33 ? QColor("#ee9a3a")
                                           : QColor("#3fa37a");
        p.setBrush(color);
        p.drawRoundedRect(QRectF(1.0, y + 1.0, fw, barH - 1.0), 2.0, 2.0);
    };
    fill(mic_, top);
    fill(remote_, top + barH + gap);
}