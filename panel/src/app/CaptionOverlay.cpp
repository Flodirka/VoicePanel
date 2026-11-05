#include "app/CaptionOverlay.h"
#include "app/PanelApp.h"
#include "app/UiKit.h"
#include "audio/WasapiCapture.h"
#include "core/SessionController.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QVBoxLayout>

namespace VoicePanel {

CaptionOverlay::CaptionOverlay(PanelApp *app, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), app_(app) {
    // A stylesheet background is only painted when the widget is told to draw
    // a styled background; without it the strip was transparent and the cream
    // text vanished over video.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    setObjectName(QStringLiteral("captionOverlay"));
    setWindowIcon(UiKit::icon("mic"));
    setWindowTitle(tr("VoicePanel — subtitles"));
    resize(900, 168);

    controller_ = new SessionController(app_->service(), app_->tts(), this);
    connect(controller_, &SessionController::caption, this,
            [this](const QString &heard, const QString &translated) {
        heard_->setText(heard);
        translated_->setText(translated);
    });
    connect(controller_, &SessionController::error, this,
            [this](const QString &message) { translated_->setText(message); });

    buildUi();
    refreshSources();
}

CaptionOverlay::~CaptionOverlay() { end(); }

void CaptionOverlay::buildUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(8);

    // Visible controls: source, direction, voice. No hidden right-click menu.
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    source_ = new QComboBox;
    source_->setMinimumWidth(220);
    source_->setToolTip(tr("Which audio is captioned: an application, or the whole output"));
    row->addWidget(source_);
    direction_ = new QComboBox;
    direction_->addItem(tr("English → Russian"), int(0));
    direction_->addItem(tr("Russian → English"), int(1));
    row->addWidget(direction_);
    voice_ = new QCheckBox(tr("Voice"));
    voice_->setToolTip(tr("Speak the translation with the cloned voice"));
    row->addWidget(voice_);
    row->addStretch();
    close_ = new QPushButton;
    close_->setProperty("variant", "ghost");
    close_->setProperty("size", "icon");
    close_->setIcon(UiKit::icon("close"));
    close_->setFixedSize(28, 28);
    close_->setIconSize(QSize(16, 16));
    close_->setToolTip(tr("Stop captioning (Esc)"));
    row->addWidget(close_);
    layout->addLayout(row);

    heard_ = new QLabel(tr("Choose an application above, then play the video."));
    heard_->setObjectName(QStringLiteral("captionHeard"));
    heard_->setWordWrap(true);
    QFont small = heard_->font();
    small.setPointSize(10);
    heard_->setFont(small);
    layout->addWidget(heard_);

    translated_ = new QLabel(QString());
    translated_->setObjectName(QStringLiteral("captionTranslated"));
    translated_->setWordWrap(true);
    translated_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    QFont big = translated_->font();
    big.setPointSize(17);
    big.setWeight(QFont::DemiBold);
    translated_->setFont(big);
    layout->addWidget(translated_, 1);

    // An opaque card: cream text over a bright video was unreadable.
    setStyleSheet(QStringLiteral(
        "#captionHeard { color: #c9bd9a; background: transparent; }"
        "#captionTranslated { color: #faf1cc; background: transparent; }"
        "#captionOverlay QComboBox { background-color: #19141f; color: #faf1cc;"
        " border: 1px solid rgba(250, 241, 204, 0.18); border-radius: 8px; padding: 4px 8px; }"
        "#captionOverlay QCheckBox { color: #faf1cc; background: transparent; }"
        "#captionOverlay QLabel { color: #faf1cc; background: transparent; }"));

    connect(close_, &QPushButton::clicked, this, [this] {
        end();
        hide();
    });
    connect(source_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        applySettings();
        restart();
    });
    connect(direction_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        applySettings();
        restart();
    });
    connect(voice_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_) return;
        app_->settings()->speakCaptions = on;
        app_->settings()->save();
        controller_->setSpeakCaptions(on);
    });
}

void CaptionOverlay::refreshSources() {
    loading_ = true;
    source_->clear();
    const auto apps = WasapiCapture::processLoopbackSupported()
                          ? WasapiCapture::listAudioProcesses()
                          : QList<QPair<quint32, QString>>();
    for (const auto &app : apps) {
        source_->addItem(app.second, QStringLiteral("pid:%1").arg(app.first));
    }
    if (!apps.isEmpty()) {
        source_->insertSeparator(source_->count());
    }
    for (const auto &device : WasapiCapture::listDevices(false)) {
        source_->addItem(device.second.isEmpty() ? tr("Whole output (default)")
                                                 : tr("Whole output — %1").arg(device.second),
                         device.first.isEmpty() ? QStringLiteral("dev:") : QStringLiteral("dev:") + device.first);
    }

    const Settings *s = app_->settings();
    const QString saved = s->systemAudioDevice;
    const QString wanted = saved.isEmpty() ? QStringLiteral("dev:")
                         : (saved.startsWith(QLatin1String("dev:")) ? saved
                                                                    : QStringLiteral("pid:") + saved);
    int row = source_->findData(wanted);
    if (row < 0) row = source_->findData(QStringLiteral("dev:"));
    if (row >= 0) source_->setCurrentIndex(row);

    direction_->setCurrentIndex(direction_->findData(s->subtitlesLanguage));
    voice_->setChecked(s->speakCaptions);
    loading_ = false;
}

void CaptionOverlay::applySettings() {
    Settings *s = app_->settings();
    const QString source = source_->currentData().toString();
    s->systemAudioDevice = source.startsWith(QLatin1String("pid:")) ? source
                                                                   : source.mid(4);
    s->subtitlesLanguage = direction_->currentData().toInt();
    s->speakCaptions = voice_->isChecked();
    s->save();
}

void CaptionOverlay::restart() {
    if (!running_) return;
    end();
    begin();
}

void CaptionOverlay::begin() {
    if (running_) return;
    // A fresh list each time: applications come and go.
    const QString keep = source_->currentData().toString();
    refreshSources();
    if (int row = source_->findData(keep); row >= 0) source_->setCurrentIndex(row);

    controller_->configure(*app_->settings(), SessionController::Mode::Subtitles);
    controller_->setDirection(direction_->currentData().toInt());
    controller_->setSpeakCaptions(voice_->isChecked());
    QString vadPath = app_->vadPath();
    if (vadPath.isEmpty()) vadPath = app_->settings()->vadModel;
    if (vadPath.isEmpty()) vadPath = QStringLiteral("models/silero_vad.onnx");
    if (!controller_->start(vadPath)) {
        translated_->setText(tr("Could not capture that source — pick another one above."));
        show();
        raise();
        return;
    }
    running_ = true;
    show();
    raise();
}

void CaptionOverlay::end() {
    if (!running_) return;
    controller_->stop();
    running_ = false;
}

void CaptionOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(250, 241, 204, 60), 1.0));
    painter.setBrush(QColor(17, 14, 22, 248));   // surface-0, near opaque
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
}

void CaptionOverlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        end();
        hide();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CaptionOverlay::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CaptionOverlay::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void CaptionOverlay::mouseReleaseEvent(QMouseEvent *event) {
    dragging_ = false;
    QWidget::mouseReleaseEvent(event);
}

} // namespace VoicePanel
