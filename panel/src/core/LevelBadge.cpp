#include "LevelBadge.h"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>

namespace VoicePanel {

LevelBadge::LevelBadge(Kind kind, QWidget *parent)
    : QWidget(parent), kind_(kind) {
    setFixedSize(kBadge, kBadge);
    decay_ = new QTimer(this);
    decay_->setInterval(60);
    connect(decay_, &QTimer::timeout, this, &LevelBadge::refresh);
    // The timer only runs while a session is active: an idle widget must not
    // wake the loop just to repaint the same idle glyph.
    decay_->start();
}

void LevelBadge::setLevel(double normalized) {
    const double clamped = qBound(0.0, normalized, 1.0);
    // Fast attack so speech reads immediately, slow decay so the glyph does
    // not flicker between words.
    level_ = clamped > level_ ? level_ + (clamped - level_) * 0.7
                              : level_ + (clamped - level_) * 0.35;
    update();
}

void LevelBadge::setRecording(bool recording) {
    if (recording_ == recording) return;
    recording_ = recording;
    if (!recording_) level_ = 0.0;
    update();
}

void LevelBadge::reset() {
    level_ = 0.0;
    recording_ = false;
    update();
}

void LevelBadge::refresh() {
    if (level_ <= 0.003) {
        if (level_ != 0.0) {
            level_ = 0.0;
            update();
        }
        return;
    }
    level_ *= 0.88;                        // no events -> decay to idle
    update();
}

void LevelBadge::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // Design tokens: the active/recording state is amber (badge-active-*),
    // never the error red, which the system reserves for failures.
    const QColor ink = palette().color(QPalette::WindowText);
    const QColor accent = QColor(recording_ ? "#ee9a3a" : ink.name());
    const QColor accentSoft = QColor("#f4b66e");
    painter.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    if (kind_ == Kind::Microphone) {
        // Lucide "mic": capsule body, stand, base.
        const QRectF body(8.0, 3.0, 6.0, 10.0);
        painter.drawRoundedRect(body, 3.0, 3.0);
        if (recording_ && level_ > 0.02) {
            QPainterPath clip;
            clip.addRoundedRect(body, 3.0, 3.0);
            painter.save();
            painter.setClipPath(clip);
            painter.fillRect(QRectF(body.left(), body.bottom() - body.height() * level_,
                                    body.width(), body.height() * level_), accent);
            painter.restore();
        }
        painter.drawArc(QRectF(5.0, 5.0, 12.0, 12.0), 180 * 16, 180 * 16);
        painter.drawLine(QPointF(12.0, 17.0), QPointF(12.0, 20.0));
        painter.drawLine(QPointF(9.0, 20.5), QPointF(15.0, 20.5));
    } else {
        // Lucide "volume-2": speaker wedge with waves; the waves thicken with
        // the remote level so the two badges read differently at a glance.
        QPainterPath wedge;
        wedge.moveTo(4.0, 9.5);
        wedge.lineTo(8.0, 9.5);
        wedge.lineTo(12.0, 5.5);
        wedge.lineTo(12.0, 16.5);
        wedge.lineTo(8.0, 12.5);
        wedge.lineTo(4.0, 12.5);
        wedge.closeSubpath();
        painter.drawPath(wedge);
        painter.setPen(QPen(level_ > 0.10 ? accent : ink, 1.7, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(11.0, 6.5, 7.0, 9.0), -55 * 16, 110 * 16);
        if (level_ > 0.45)
            painter.drawArc(QRectF(13.0, 4.5, 10.0, 13.0), -55 * 16, 110 * 16);
        // Small level dot under the wedge: the same visual language as the
        // microphone fill, so both sides report loudness consistently.
        if (recording_ && level_ > 0.02) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawEllipse(QPointF(12.0, 20.0), 1.6 + 1.2 * level_, 1.6 + 1.2 * level_);
        }
    }
}

} // namespace VoicePanel
