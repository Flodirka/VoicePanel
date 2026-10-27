#include "app/UiKit.h"

#include <QDialog>
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWidget>

static void initResources() { Q_INIT_RESOURCE(resources); }

namespace VoicePanel::UiKit {

void applyTheme() {
    initResources();
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QFontDatabase::addApplicationFont(":/fonts/Inter-Variable.ttf");
    QFont font("Inter");
    font.setPixelSize(14);
    QApplication::setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#110e16"));
    palette.setColor(QPalette::WindowText, QColor("#faf1cc"));
    palette.setColor(QPalette::Base, QColor("#201b27"));
    palette.setColor(QPalette::Text, QColor("#faf1cc"));
    palette.setColor(QPalette::Button, QColor("#201b27"));
    palette.setColor(QPalette::ButtonText, QColor("#faf1cc"));
    palette.setColor(QPalette::Highlight, QColor("#ee9a3a"));
    palette.setColor(QPalette::HighlightedText, QColor("#110e16"));
    QApplication::setPalette(palette);
    QFile theme(":/theme.qss");
    if (theme.open(QIODevice::ReadOnly)) qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
}

QIcon icon(const QString &name, const QColor &color) {
    QFile file(":/icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll().replace("currentColor", color.name().toUtf8());
    QSvgRenderer renderer(svg);
    QIcon result;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

QIcon microphoneIcon(double level, bool recording) {
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);
    const QColor ink("#110e16");
    painter.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(9, 2, 6, 13), 3, 3);
    if (recording) {
        QPainterPath body;
        body.addRoundedRect(QRectF(9, 2, 6, 13), 3, 3);
        painter.save();
        painter.setClipPath(body);
        painter.fillRect(QRectF(9, 15 - 13 * level, 6, 13 * level), ink);
        painter.restore();
    }
    painter.drawArc(QRectF(5, 5, 14, 14), 180 * 16, 180 * 16);
    painter.drawLine(QPointF(5, 10), QPointF(5, 12));
    painter.drawLine(QPointF(19, 10), QPointF(19, 12));
    painter.drawLine(QPointF(12, 19), QPointF(12, 22));
    if (recording) {
        painter.setBrush(ink);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(21, 3), 1.5, 1.5);
    }
    painter.end();
    return QIcon(pixmap);
}

QWidget *wrapDialog(QDialog &dlg, const QString &title) {
    // The shell becomes the dialog's top-level layout so the existing content
    // is preserved; a themed header row with the title and a close button
    // replaces the native frame, which blended into a dark desktop.
    dlg.setWindowFlags(dlg.windowFlags() | Qt::FramelessWindowHint);
    dlg.setAttribute(Qt::WA_TranslucentBackground);
    auto *body = new QWidget;
    if (dlg.layout()) body->setLayout(dlg.layout());
    auto *shell = new QWidget(&dlg);
    shell->setObjectName("dialogShell");
    auto *outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(shell, 1);

    auto *shellLay = new QVBoxLayout(shell);
    shellLay->setContentsMargins(0, 0, 0, 0);
    shellLay->setSpacing(0);

    auto *header = new QWidget(shell);
    header->setObjectName("dialogHeader");
    header->setFixedHeight(44);
    auto *headLay = new QHBoxLayout(header);
    headLay->setContentsMargins(16, 0, 12, 0);
    headLay->setSpacing(8);
    auto *titleLabel = new QLabel(title, header);
    titleLabel->setProperty("role", "dialogTitle");
    headLay->addWidget(titleLabel, 1);
    auto *closeBtn = new QPushButton(header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "icon");
    closeBtn->setFixedSize(30, 30);
    closeBtn->setIcon(icon("close"));
    closeBtn->setIconSize(QSize(14, 14));
    closeBtn->setToolTip(QObject::tr("Close"));
    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    headLay->addWidget(closeBtn);
    shellLay->addWidget(header);

    // Re-parent the dialog's pre-existing layout items (if any) into the shell,
    // below the header, so callers can build the body first and wrap it later.
    shellLay->addWidget(body, 1);

    // The header row doubles as the drag handle so the frameless window moves.
    struct DragFilter : public QObject {
        explicit DragFilter(QObject *parent) : QObject(parent) {}
        QPoint offset;
        bool dragging = false;
        bool eventFilter(QObject *watched, QEvent *event) override {
            QWidget *h = qobject_cast<QWidget *>(watched);
            if (!h) return false;
            if (event->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(event);
                if (me->button() == Qt::LeftButton) {
                    dragging = true;
                    offset = me->globalPosition().toPoint() - h->window()->frameGeometry().topLeft();
                    event->accept();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove && dragging) {
                auto *me = static_cast<QMouseEvent *>(event);
                h->window()->move(me->globalPosition().toPoint() - offset);
                event->accept();
                return true;
            } else if (event->type() == QEvent::MouseButtonRelease) {
                dragging = false;
                return true;
            }
            return false;
        }
    };
    auto *filter = new DragFilter(&dlg);
    header->installEventFilter(filter);
    titleLabel->installEventFilter(filter);
    return shell;
}

} // namespace VoicePanel::UiKit
