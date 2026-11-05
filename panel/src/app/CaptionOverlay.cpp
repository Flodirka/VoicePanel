#include "app/CaptionOverlay.h"
#include "app/PanelApp.h"
#include "app/SettingsPopover.h"
#include "app/UiKit.h"
#include "audio/WasapiCapture.h"
#include "core/LevelBadge.h"
#include "core/SessionController.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
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
    resize(560, 150);

    controller_ = new SessionController(app_->service(), app_->tts(), this);
    connect(controller_, &SessionController::caption, this,
            [this](const QString &heard, const QString &translated) {
        heard_->setText(heard);
        translated_->setText(translated);
    });
    connect(controller_, &SessionController::error, this,
            [this](const QString &message) { translated_->setText(message); });

    buildUi();
    buildSettings();
    refreshSources();
    applyDisplayMode();
    applyTextSize();
}

CaptionOverlay::~CaptionOverlay() { end(); }

void CaptionOverlay::buildUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(0);

    // Compact toolbar: settings, direction, then state and close. Everything
    // else lives in the popover, so the strip stays caption-sized.
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    auto *settingsButton = new QPushButton;
    settingsButton->setProperty("variant", "ghost");
    settingsButton->setProperty("size", "icon");
    settingsButton->setIcon(UiKit::icon("cog"));
    settingsButton->setFixedSize(28, 28);
    settingsButton->setIconSize(QSize(16, 16));
    settingsButton->setToolTip(tr("Subtitles settings"));
    row->addWidget(settingsButton);

    direction_ = new QComboBox;
    direction_->addItem(tr("English → Russian"), int(0));
    direction_->addItem(tr("Russian → English"), int(1));
    direction_->setToolTip(tr("Direction"));
    row->addWidget(direction_);

    row->addStretch();

    micBadge_ = new LevelBadge(LevelBadge::Kind::Microphone);
    micBadge_->setObjectName(QStringLiteral("captionMicBadge"));
    micBadge_->setToolTip(tr("Captioning"));
    row->addWidget(micBadge_);

    close_ = new QPushButton;
    close_->setProperty("variant", "ghost");
    close_->setProperty("size", "icon");
    close_->setIcon(UiKit::icon("close"));
    close_->setFixedSize(28, 28);
    close_->setIconSize(QSize(16, 16));
    close_->setToolTip(tr("Stop captioning (Esc)"));
    row->addWidget(close_);
    layout->addLayout(row);
    settingsButton_ = settingsButton;

    // Caption card: the heard line muted, a hairline, the translation large —
    // the mobile-subtitles layout the design was approved against.
    auto *card = new QVBoxLayout;
    card->setContentsMargins(14, 2, 14, 4);
    card->setSpacing(0);
    heard_ = new QLabel(tr("Choose a source in settings, then play the audio."));
    heard_->setObjectName(QStringLiteral("captionHeard"));
    heard_->setWordWrap(true);
    heard_->setMaximumHeight(44);
    card->addWidget(heard_);

    divider_ = new QFrame;
    divider_->setFrameShape(QFrame::HLine);
    divider_->setFrameShadow(QFrame::Plain);
    divider_->setFixedHeight(1);
    divider_->setStyleSheet(QStringLiteral("background: rgba(250, 241, 204, 0.16);"));
    card->addWidget(divider_);

    translated_ = new QLabel(QString());
    translated_->setObjectName(QStringLiteral("captionTranslated"));
    translated_->setWordWrap(true);
    translated_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    translated_->setMaximumHeight(64);
    card->addWidget(translated_);
    layout->addLayout(card, 1);

    setStyleSheet(QStringLiteral(
        "#captionHeard { color: #c9bd9a; background: transparent; }"
        "#captionTranslated { color: #faf1cc; background: transparent; }"
        "#captionOverlay QComboBox { background-color: #19141f; color: #faf1cc;"
        " border: 1px solid rgba(250, 241, 204, 0.16); border-radius: 8px; padding: 3px 8px; }"
        "#captionOverlay QLabel { color: #faf1cc; background: transparent; }"));

    connect(close_, &QPushButton::clicked, this, [this] {
        end();
        hide();
    });
    connect(direction_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        applySettings();
        restart();
    });
}

void CaptionOverlay::buildSettings() {
    settings_ = new SettingsPopover(settingsButton_, tr("Subtitles settings"));
    auto *form = settings_->form();

    source_ = new QComboBox;
    source_->setMinimumContentsLength(24);
    source_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    source_->setToolTip(tr("Which audio is captioned: an application, or the whole output"));
    form->addRow(tr("Audio source"), source_);

    display_ = new QComboBox;
    display_->addItem(tr("Transcript and translation"), int(0));
    display_->addItem(tr("Translation only"), int(1));
    display_->addItem(tr("Transcript only"), int(2));
    form->addRow(tr("Show"), display_);

    size_ = new QComboBox;
    size_->addItem(tr("S"), int(0));
    size_->addItem(tr("M"), int(1));
    size_->addItem(tr("L"), int(2));
    form->addRow(tr("Text size"), size_);

    voice_ = new QCheckBox(tr("Speak the translation"));
    voice_->setToolTip(tr("Speak the translation with the selected voice"));
    form->addRow(QString(), voice_);

    connect(settingsButton_, &QPushButton::clicked, this, [this] { settings_->openAt(); });
    connect(source_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        applySettings();
        restart();
    });
    connect(display_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        app_->settings()->subtitlesDisplay = display_->currentData().toInt();
        app_->settings()->save();
        applyDisplayMode();
    });
    connect(size_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        app_->settings()->subtitlesTextSize = size_->currentData().toInt();
        app_->settings()->save();
        applyTextSize();
    });
    connect(voice_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_) return;
        app_->settings()->speakCaptions = on;
        app_->settings()->save();
        controller_->setSpeakCaptions(on);
    });
}

void CaptionOverlay::applyDisplayMode() {
    const int mode = app_->settings()->subtitlesDisplay;
    // Transcript-only reads at the translation size: the caption is the main
    // content in that mode, not a footnote.
    const bool showHeard = mode == 0 || mode == 2;
    const bool showTranslated = mode == 0 || mode == 1;
    heard_->setVisible(showHeard);
    translated_->setVisible(showTranslated);
    divider_->setVisible(mode == 0);
    QFont big = translated_->font();
    big.setPixelSize(textSizes().translated);
    big.setWeight(QFont::DemiBold);
    if (mode == 2) heard_->setFont(big);
    else applyTextSize();
}

void CaptionOverlay::applyTextSize() {
    const auto sizes = textSizes();
    QFont small = heard_->font();
    small.setPixelSize(sizes.heard);
    heard_->setFont(small);
    QFont big = translated_->font();
    big.setPixelSize(sizes.translated);
    big.setWeight(QFont::DemiBold);
    translated_->setFont(big);
    // Two lines max for either line, so the strip never grows mid-video.
    heard_->setMaximumHeight(sizes.heard * 3);
    translated_->setMaximumHeight(sizes.translated * 3);
}

CaptionOverlay::TextSizes CaptionOverlay::textSizes() const {
    switch (app_->settings()->subtitlesTextSize) {
    case 0: return {12, 17};
    case 2: return {19, 27};
    default: return {15, 21};
    }
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
    display_->setCurrentIndex(display_->findData(s->subtitlesDisplay));
    size_->setCurrentIndex(size_->findData(s->subtitlesTextSize));
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
        translated_->setText(tr("Could not capture that source — pick another one in settings."));
        show();
        raise();
        return;
    }
    running_ = true;
    micBadge_->setRecording(true);
    emit recordingChanged(true);
    show();
    raise();
}

void CaptionOverlay::end() {
    if (!running_) return;
    controller_->stop();
    running_ = false;
    micBadge_->reset();
    emit recordingChanged(false);
}

void CaptionOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(250, 241, 204, 40), 1.0));
    painter.setBrush(QColor(17, 14, 22, 238));   // surface-0, near opaque
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
