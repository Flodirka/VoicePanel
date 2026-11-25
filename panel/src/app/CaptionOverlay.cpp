#include "app/CaptionOverlay.h"
#include "app/PanelApp.h"
#include "app/SettingsPopover.h"
#include "app/UiKit.h"
#include "audio/WasapiCapture.h"
#include "core/LevelBadge.h"
#include "core/SessionController.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QScrollBar>
#include <QSizeGrip>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextOption>
#include <QVBoxLayout>

namespace VoicePanel {

namespace {

constexpr int kMinimumWidth = 360;
constexpr int kMinimumHeight = 130;
constexpr int kResizeBorder = 8;
constexpr int kBlockGap = 8;          // px between caption blocks
// The phrase id travels with its block, so a late speaker label can replace
// the prefix of the exact caption it belongs to.
constexpr int kPhraseIdProperty = QTextFormat::UserProperty + 1;
const QColor kCaptionInk(250, 241, 204);
const QColor kHeardInk(201, 189, 154);
const QColor kNoticeInk(224, 145, 127);

// A run of caption text with its own size and weight: the heard line is the
// footnote, the translation is the caption. Colours and sizes are explicit so
// the card never depends on the application theme's QTextEdit font.
void insertRun(QTextCursor &cursor, const QString &text, int pixelSize,
               const QColor &color, bool bold) {
    QTextCharFormat format;
    format.setForeground(color);
    QFont font = cursor.document()->defaultFont();
    font.setPixelSize(pixelSize);
    font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    format.setFont(font);
    cursor.insertText(text, format);
}

Qt::CursorShape cursorFor(Qt::Edges edges) {
    const bool left = edges & Qt::LeftEdge;
    const bool right = edges & Qt::RightEdge;
    const bool top = edges & Qt::TopEdge;
    const bool bottom = edges & Qt::BottomEdge;
    if ((left && top) || (right && bottom)) return Qt::SizeFDiagCursor;
    if ((right && top) || (left && bottom)) return Qt::SizeBDiagCursor;
    if (left || right) return Qt::SizeHorCursor;
    if (top || bottom) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

} // namespace

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
    // A frameless window has no system border to drag, so it needs its own
    // minimum size and remembers the size the user chose.
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    resize(620, 220);
    setMouseTracking(true);
    if (!app_->settings()->subtitlesGeometry.isEmpty()) {
        restoreGeometry(app_->settings()->subtitlesGeometry);
    }

    controller_ = new SessionController(app_->service(), app_->tts(), this);
    connect(controller_, &SessionController::caption, this,
            [this](const QString &speaker, const QString &heard, const QString &translated,
                   quint64 id) { addCaption(speaker, heard, translated, id); });
    connect(controller_, &SessionController::captionSpeaker, this,
            [this](quint64 id, const QString &speaker) { relabel(id, speaker); });
    connect(controller_, &SessionController::error, this,
            [this](const QString &message) { addNotice(message); });
    connect(controller_, &SessionController::captureFailed, this,
            [this](const QString &message) {
        // Captions cannot continue without the source; leaving the badge
        // "recording" over a dead stream was the worst part of a failed start.
        end();
        addNotice(tr("Capture stopped: %1").arg(message));
    });

    buildUi();
    buildSettings();
    refreshSources();
    applyDisplayMode();
    applyTextSize();
}

CaptionOverlay::~CaptionOverlay() {
    end();
    saveGeometryToSettings();
}

void CaptionOverlay::buildUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

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

    // Caption card: a scrolling transcript, the heard line muted above the
    // translation. It replaces the two fixed-height labels that clipped long
    // phrases: every caption is kept, the newest one stays in view, and the
    // user can scroll back through what was said.
    captions_ = new QTextEdit;
    captions_->setObjectName(QStringLiteral("captionView"));
    captions_->setReadOnly(true);
    captions_->setAcceptRichText(true);
    captions_->setFrameShape(QFrame::NoFrame);
    captions_->setLineWrapMode(QTextEdit::WidgetWidth);
    captions_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    captions_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    captions_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    captions_->setFocusPolicy(Qt::NoFocus);
    captions_->setMinimumHeight(48);
    captions_->document()->setDocumentMargin(0);
    QPalette palette = captions_->palette();
    palette.setColor(QPalette::Base, Qt::transparent);
    palette.setColor(QPalette::Text, kCaptionInk);
    captions_->setPalette(palette);
    captions_->viewport()->setAutoFillBackground(false);
    layout->addWidget(captions_, 1);

    // The bottom row carries the grip: with a frameless window this is the
    // visible hint that the panel can be resized at all.
    auto *foot = new QHBoxLayout;
    foot->setContentsMargins(0, 0, 0, 0);
    foot->setSpacing(0);
    foot->addStretch();
    grip_ = new QSizeGrip(this);
    grip_->setObjectName(QStringLiteral("captionGrip"));
    grip_->setFixedSize(14, 14);
    grip_->setToolTip(tr("Drag to resize"));
    foot->addWidget(grip_);
    layout->addLayout(foot);

    setStyleSheet(QStringLiteral(
        "#captionView { background: transparent; border: none; padding: 0;"
        " color: #faf1cc; }"
        "#captionOverlay QComboBox { background-color: #19141f; color: #faf1cc;"
        " border: 1px solid rgba(250, 241, 204, 0.16); border-radius: 8px; padding: 3px 8px; }"
        "#captionOverlay QLabel { color: #faf1cc; background: transparent; }"));

    showPlaceholder();

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

    // Caption voicing is deferred: the voice would be captured again by the
    // same loopback the captions translate from, so it waits for a playback
    // route that excludes our own output. Subtitles stay text-only.

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
}

void CaptionOverlay::applyDisplayMode() {
    // Both display mode and text size decide what each block looks like, so
    // the card is re-rendered from the kept captions instead of only changing
    // what future phrases show.
    renderCaptions();
}

void CaptionOverlay::applyTextSize() {
    const auto sizes = textSizes();
    QFont base = captions_->font();
    base.setPixelSize(sizes.translated);
    captions_->setFont(base);
    captions_->document()->setDefaultFont(base);
    // The application theme styles QTextEdit; an ID selector on the card wins,
    // and it carries the chosen size so the theme cannot shrink the captions.
    captions_->setStyleSheet(QStringLiteral(
        "#captionView { background: transparent; border: none; padding: 0;"
        " color: #faf1cc; font-size: %1px; }").arg(sizes.translated));
    renderCaptions();
}

CaptionOverlay::TextSizes CaptionOverlay::textSizes() const {
    switch (app_->settings()->subtitlesTextSize) {
    case 0: return {12, 17};
    case 2: return {19, 27};
    default: return {15, 21};
    }
}

void CaptionOverlay::showPlaceholder() {
    QTextCursor cursor(captions_->document());
    cursor.movePosition(QTextCursor::End);
    insertRun(cursor, tr("Choose a source in settings, then play the audio."),
              textSizes().heard, kHeardInk, false);
    placeholderShown_ = true;
}

void CaptionOverlay::clearCaptions() {
    history_.clear();
    captions_->clear();
    placeholderShown_ = false;
}

void CaptionOverlay::renderCaptions() {
    captions_->clear();
    placeholderShown_ = false;
    if (history_.isEmpty()) {
        showPlaceholder();
        return;
    }
    for (const CaptionLine &line : history_) appendBlock(line, false);
    scrollToNewest();
}

bool CaptionOverlay::following() const {
    // The newest caption is followed unless the user scrolled back to read
    // something; a caption arriving then must not yank the view away.
    const QScrollBar *bar = captions_->verticalScrollBar();
    return bar->value() >= bar->maximum() - 4;
}

void CaptionOverlay::scrollToNewest() {
    captions_->moveCursor(QTextCursor::End);
    captions_->ensureCursorVisible();
    QScrollBar *bar = captions_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void CaptionOverlay::writeRuns(QTextCursor &cursor, const CaptionLine &line) {
    const auto sizes = textSizes();
    const int mode = app_->settings()->subtitlesDisplay;
    const bool transcriptOnly = mode == 2 && !line.notice;
    const bool showHeard = line.notice || mode == 0 || mode == 2;
    const bool showTranslated = !line.notice && (mode == 0 || mode == 1);
    const QString tag = line.speaker.isEmpty() ? QString()
                                               : line.speaker + QStringLiteral(": ");

    if (line.notice) {
        insertRun(cursor, line.heard, sizes.heard, kNoticeInk, false);
        return;
    }
    if (showHeard) {
        // Transcript-only makes the caption the main content, so it reads at
        // the translation size instead of the footnote size.
        insertRun(cursor, tag + line.heard,
                  transcriptOnly ? sizes.translated : sizes.heard,
                  transcriptOnly ? kCaptionInk : kHeardInk,
                  transcriptOnly);
        // One block per caption: the two lines are separated inside it, so a
        // late speaker label rewrites just this block.
        if (showTranslated) {
            QTextCharFormat separator;
            separator.setForeground(kHeardInk);
            cursor.insertText(QString(QChar(QChar::LineSeparator)), separator);
        }
    }
    if (showTranslated) {
        insertRun(cursor, showHeard ? line.translated : tag + line.translated,
                  sizes.translated, kCaptionInk, true);
    }
}

void CaptionOverlay::appendBlock(const CaptionLine &line, bool follow) {
    QTextCursor cursor(captions_->document());
    cursor.movePosition(QTextCursor::End);
    QTextBlockFormat block;
    block.setBottomMargin(kBlockGap);
    block.setProperty(kPhraseIdProperty, qlonglong(line.id));
    if (captions_->document()->isEmpty()) {
        cursor.setBlockFormat(block);
    } else {
        cursor.insertBlock(block);
    }
    writeRuns(cursor, line);
    if (follow) scrollToNewest();
}

void CaptionOverlay::addCaption(const QString &speaker, const QString &heard,
                                const QString &translated, quint64 id) {
    if (heard.isEmpty() && translated.isEmpty()) return;
    const bool follow = following();
    if (placeholderShown_) {
        captions_->clear();
        placeholderShown_ = false;
    }
    CaptionLine line;
    line.id = id;
    line.speaker = speaker;
    line.heard = heard;
    line.translated = translated;
    history_.append(line);
    appendBlock(line, follow);
}

void CaptionOverlay::relabel(quint64 id, const QString &speaker) {
    if (!id) return;
    int index = -1;
    for (int i = 0; i < history_.size(); ++i) {
        if (history_[i].id == id) {
            index = i;
            break;
        }
    }
    if (index < 0 || history_[index].speaker == speaker) return;
    history_[index].speaker = speaker;
    paintLine(id, history_[index]);
}

void CaptionOverlay::paintLine(quint64 id, const CaptionLine &line) {
    // Repaint that caption's block in place: the words and the reader's
    // scroll position stay where they are.
    for (QTextBlock block = captions_->document()->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().property(kPhraseIdProperty).toLongLong() != qlonglong(id)) continue;
        const bool follow = following();
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        writeRuns(cursor, line);
        if (follow) scrollToNewest();
        return;
    }
}

void CaptionOverlay::addNotice(const QString &text) {
    if (text.isEmpty()) return;
    const bool follow = following();
    if (placeholderShown_) {
        captions_->clear();
        placeholderShown_ = false;
    }
    CaptionLine line;
    line.heard = text;
    line.notice = true;
    history_.append(line);
    appendBlock(line, follow);
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
    loading_ = false;
}

void CaptionOverlay::applySettings() {
    Settings *s = app_->settings();
    const QString source = source_->currentData().toString();
    s->systemAudioDevice = source.startsWith(QLatin1String("pid:")) ? source
                                                                   : source.mid(4);
    s->subtitlesLanguage = direction_->currentData().toInt();
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

    // A new session starts with a clean card: captions from another source or
    // an earlier run would read as if they had just been spoken.
    clearCaptions();

    controller_->configure(*app_->settings(), SessionController::Mode::Subtitles);
    controller_->setDirection(direction_->currentData().toInt());
    QString vadPath = app_->vadPath();
    if (vadPath.isEmpty()) vadPath = app_->settings()->vadModel;
    if (vadPath.isEmpty()) vadPath = QStringLiteral("models/silero_vad.onnx");
    if (!controller_->start(vadPath)) {
        addNotice(tr("Could not capture that source — pick another one in settings."));
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

    // The platform's size grip is drawn in the system's dark ink, which is
    // invisible on this card. Three hairlines in the corner say "drag me".
    painter.setPen(QPen(QColor(250, 241, 204, 80), 1.2));
    for (int i = 1; i <= 3; ++i) {
        painter.drawLine(width() - 4 - i * 4, height() - 4,
                         width() - 4, height() - 4 - i * 4);
    }
}

Qt::Edges CaptionOverlay::edgesAt(const QPoint &pos) const {
    Qt::Edges edges;
    if (pos.x() <= kResizeBorder) edges |= Qt::LeftEdge;
    if (pos.x() >= width() - kResizeBorder) edges |= Qt::RightEdge;
    if (pos.y() <= kResizeBorder) edges |= Qt::TopEdge;
    if (pos.y() >= height() - kResizeBorder) edges |= Qt::BottomEdge;
    return edges;
}

void CaptionOverlay::updateHoverCursor(const QPoint &pos) {
    const Qt::Edges edges = edgesAt(pos);
    // The resize border is the outer margin: the caption view sits inside it,
    // so it owns the pointer over the text and the text keeps its own cursor.
    if (edges == Qt::Edges()) unsetCursor();
    else setCursor(cursorFor(edges));
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
        const Qt::Edges edges = edgesAt(event->position().toPoint());
        if (edges != Qt::Edges()) {
            resizeEdges_ = edges;
            resizeStartGeometry_ = geometry();
            resizeStartGlobal_ = event->globalPosition().toPoint();
            event->accept();
            return;
        }
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CaptionOverlay::mouseMoveEvent(QMouseEvent *event) {
    if (resizeEdges_ != Qt::Edges() && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->globalPosition().toPoint() - resizeStartGlobal_;
        QRect rect = resizeStartGeometry_;
        const int minW = minimumWidth();
        const int minH = minimumHeight();
        if (resizeEdges_ & Qt::LeftEdge) {
            rect.setLeft(qMin(rect.left() + delta.x(), rect.right() - minW + 1));
        }
        if (resizeEdges_ & Qt::RightEdge) {
            rect.setRight(qMax(rect.right() + delta.x(), rect.left() + minW - 1));
        }
        if (resizeEdges_ & Qt::TopEdge) {
            rect.setTop(qMin(rect.top() + delta.y(), rect.bottom() - minH + 1));
        }
        if (resizeEdges_ & Qt::BottomEdge) {
            rect.setBottom(qMax(rect.bottom() + delta.y(), rect.top() + minH - 1));
        }
        setGeometry(rect);
        event->accept();
        return;
    }
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }
    updateHoverCursor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void CaptionOverlay::mouseReleaseEvent(QMouseEvent *event) {
    const bool changed = dragging_ || resizeEdges_ != Qt::Edges();
    dragging_ = false;
    resizeEdges_ = Qt::Edges();
    if (changed) {
        updateHoverCursor(event->position().toPoint());
        saveGeometryToSettings();
    }
    QWidget::mouseReleaseEvent(event);
}

void CaptionOverlay::leaveEvent(QEvent *event) {
    if (!dragging_ && resizeEdges_ == Qt::Edges()) unsetCursor();
    QWidget::leaveEvent(event);
}

void CaptionOverlay::hideEvent(QHideEvent *event) {
    saveGeometryToSettings();
    QWidget::hideEvent(event);
}

void CaptionOverlay::saveGeometryToSettings() {
    if (!app_ || !app_->settings()) return;
    // The strip is frameless, so this is the only place its size can come
    // from: the user drags it to the size their video needs.
    app_->settings()->subtitlesGeometry = saveGeometry();
    app_->settings()->save();
}

} // namespace VoicePanel
