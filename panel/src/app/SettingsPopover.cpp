#include "app/SettingsPopover.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QGuiApplication>
#include <QVBoxLayout>

namespace VoicePanel {

SettingsPopover::SettingsPopover(QWidget *anchor, const QString &title)
    : QDialog(anchor ? anchor->window() : nullptr,
              Qt::Popup | Qt::FramelessWindowHint),
      anchor_(anchor) {
    // Qt::Popup closes on any outside click, which is exactly the mini-modal
    // behaviour of the reference designs; Esc and the close button also work.
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(false);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 12, 16, 16);
    lay->setSpacing(10);

    auto *header = new QLabel(title, this);
    header->setProperty("role", "dialogTitle");
    lay->addWidget(header);

    column_ = new QVBoxLayout;
    column_->setSpacing(8);
    lay->addLayout(column_);

    form_ = new QFormLayout;
    form_->setHorizontalSpacing(16);
    form_->setVerticalSpacing(10);
    column_->addLayout(form_);

    setStyleSheet(QStringLiteral(
        "#settingsPopover QLabel { color: #faf1cc; background: transparent; }"
        "#settingsPopover QComboBox { background-color: #19141f; color: #faf1cc;"
        " border: 1px solid rgba(250, 241, 204, 0.16); border-radius: 8px; padding: 4px 8px; }"
        "#settingsPopover QCheckBox { color: #faf1cc; background: transparent; }"
        "#settingsPopover QPushButton { background-color: #19141f; color: #faf1cc;"
        " border: 1px solid rgba(250, 241, 204, 0.16); border-radius: 8px; padding: 6px 10px; }"));
    setObjectName(QStringLiteral("settingsPopover"));
}

void SettingsPopover::openAt() {
    if (!anchor_) {
        show();
        return;
    }
    adjustSize();
    const QPoint origin = anchor_->mapToGlobal(QPoint(0, anchor_->height()));
    const QScreen *screen = anchor_->screen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    QPoint pos = origin + QPoint(0, 6);
    // Below the anchor by default; above it when there is no room left.
    if (!area.isNull() && pos.y() + height() > area.bottom()) {
        const QPoint top = anchor_->mapToGlobal(QPoint(0, 0));
        pos = QPoint(pos.x(), top.y() - height() - 6);
    }
    if (!area.isNull()) {
        pos.setX(qBound(area.left() + 8, pos.x(), area.right() - width() - 8));
        pos.setY(qBound(area.top() + 8, pos.y(), area.bottom() - height() - 8));
    }
    move(pos);
    show();
}

void SettingsPopover::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    painter.fillPath(path, QColor(0x20, 0x1b, 0x27));            // surface-2
    painter.setPen(QColor(250, 241, 204, 40));                   // line-soft
    painter.drawPath(path);
}

} // namespace VoicePanel
