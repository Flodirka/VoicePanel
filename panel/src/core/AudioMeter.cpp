#include "core/AudioMeter.h"

#include <QPainter>
#include <cmath>

using namespace VoicePanel;

AudioMeter::AudioMeter(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void AudioMeter::setLevel(double rms) {
    // speech RMS hovers around 0.02-0.3; map logarithmically to 0..1
    const double db = 20.0 * std::log10(std::max(rms, 1e-6));
    const double norm = qBound(0.0, (db + 60.0) / 60.0, 1.0);
    smooth_ = smooth_ * 0.6 + norm * 0.4;
    level_ = norm;
    update();
}

void AudioMeter::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    // Track: surface-2 (#201b27); fill: success -> amber -> error tokens.
    p.setBrush(QColor("#201b27"));
    p.drawRoundedRect(rect(), 4, 4);
    const int w = int(double(width() - 4) * smooth_);
    QColor color = smooth_ > 0.66 ? QColor("#e5544e")
                 : smooth_ > 0.33 ? QColor("#ee9a3a")
                                  : QColor("#3fa37a");
    p.setBrush(color);
    p.drawRoundedRect(2, 3, w, height() - 6, 3, 3);
}
