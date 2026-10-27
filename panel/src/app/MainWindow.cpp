#include "app/MainWindow.h"
#include "app/PanelApp.h"
#include "app/SettingsDialog.h"
#include "app/UiKit.h"
#include "app/VoicePlaybackDialog.h"
#include "core/DictationController.h"
#include "core/MarkdownStore.h"
#include "core/ServiceClient.h"
#include "core/Settings.h"
#include "core/TextInserter.h"
#include "core/TextProcessor.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizeGrip>
#include <QSplitter>
#include <QStyle>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QUuid>
#include <cmath>
#include <windows.h>

using namespace VoicePanel;

MainWindow::MainWindow(PanelApp *app, QWidget *parent) : QMainWindow(parent), app_(app) {
    setWindowTitle(tr("VoicePanel"));
    setWindowIcon(UiKit::icon("mic"));
    buildUi();
    configureProcessing();
    connect(app_->dictation(), &DictationController::levelChanged, this, &MainWindow::updateMicrophone);
    connect(app_->dictation(), &DictationController::partialTranscript, this, [this](const QString &text, int id) {
        if (text.isEmpty()) previews_.remove(id);
        else previews_[id] = text;
        updateOriginal();
    });
    connect(app_->dictation(), &DictationController::finalTranscript, this, &MainWindow::onFinalTranscript);
    connect(app_->dictation(), &DictationController::speechStarted, this, [this]() {
        speechActive_ = true;
        transcriptTimer_.stop();
    });
    connect(app_->dictation(), &DictationController::speechEnded, this, [this]() {
        speechActive_ = false;
        if (recording_ && app_->settings()->transcriptResetS > 0)
            transcriptTimer_.start(app_->settings()->transcriptResetS * 1000);
    });
    connect(app_->dictation(), &DictationController::sessionEnded, this, [this](bool cancelled) {
        if (!cancelled && finishing_) app_->processor()->finish();
    });
    connect(app_->dictation(), &DictationController::error, this, [this](const QString &error) {
        processingLabel_->setText(error);
        if (recording_) stopDictation();
        showExpanded();
    });
    connect(app_->processor(), &TextProcessor::processedBatch, this, &MainWindow::onProcessedBatch);
    connect(app_->processor(), &TextProcessor::pendingChanged, this, [this](const QString &pending) {
        fragmentText_ = pending;
        updateResult();
    });
    connect(app_->processor(), &TextProcessor::fragmentRemaining, this, [this](const QString &tail) {
        fragmentText_ = tail;
        finalFragment_ = tail;
        updateResult();
    });
    connect(app_->processor(), &TextProcessor::finished, this, &MainWindow::finishSession);
    connect(app_->processor(), &TextProcessor::failed, this, [this](const QString &error) {
        processingLabel_->setText(error);
    });
    connect(&statusTimer_, &QTimer::timeout, this, &MainWindow::updateServiceStatus);
    statusTimer_.start(15000);
    // Rollover waits for recognition, processing and insertion, then saves the
    // completed document before clearing the panes. Capture stays open.
    transcriptTimer_.setSingleShot(true);
    connect(&transcriptTimer_, &QTimer::timeout, this, &MainWindow::startNewTranscript);
    insertTimer_.setSingleShot(true);
    connect(&insertTimer_, &QTimer::timeout, this, [this]() {
        // Do not paste while a dictation hotkey's modifiers are still held.
        if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT)) & 0x8000) {
            insertTimer_.start(50);
            return;
        }
        insertResult(true);
        if (finishing_ && processingDone_) finishSession();
    });
    if (!app_->settings()->compactGeometry.isEmpty()) restoreGeometry(app_->settings()->compactGeometry);
    else {
        const QRect area = screen()->availableGeometry();
        move(area.center().x() - width() / 2, area.bottom() - height() - 24);
    }
    applyDockPosition();
    changingGeometry_ = false;
    updateActions();
    updateServiceStatus();
}

MainWindow::~MainWindow() {
    app_->dictation()->cancel();
    app_->processor()->reset();
    releaseFocusTarget(target_);
    for (auto &transcript : preservedTranscripts_) releaseFocusTarget(transcript.target);
}

void MainWindow::buildUi() {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto *root = new QWidget;
    root->setObjectName("panelRoot");
    rootLayout_ = new QVBoxLayout(root);
    rootLayout_->setContentsMargins(2, 2, 2, 2);
    rootLayout_->setSpacing(0);
    compactBar_ = new QWidget;
    compactBar_->setObjectName("compactBar");
    compactBar_->installEventFilter(this);
    compactBar_->setCursor(Qt::SizeAllCursor);
    barLayout_ = new QHBoxLayout(compactBar_);
    barLayout_->setContentsMargins(10, 8, 10, 8);
    barLayout_->setSpacing(8);
    placementButton_ = new QPushButton;
    placementButton_->setObjectName("placementButton");
    placementButton_->setProperty("variant", "ghost");
    placementButton_->setProperty("size", "icon");
    placementButton_->setIcon(UiKit::icon("grip"));
    placementButton_->setToolTip(tr("Drag to move. Click for placement. Double-click to snap to the nearest edge."));
    placementButton_->setAccessibleName(tr("Panel placement"));
    placementButton_->installEventFilter(this);
    barLayout_->addWidget(placementButton_);
    micButton_ = new QPushButton;
    micButton_->setObjectName("recordButton");
    micButton_->setProperty("variant", "primary");
    micButton_->setProperty("size", "icon");
    micButton_->setFixedSize(36, 36);
    micButton_->setIconSize(QSize(24, 24));
    micButton_->setFocusPolicy(Qt::NoFocus);
    micButton_->setAccessibleName(tr("Start or stop recording"));
    connect(micButton_, &QPushButton::clicked, this, [this]() {
        toggleDictation(app_->settings()->reviewBeforeInsert || !app_->settings()->quickInsert);
    });
    barLayout_->addWidget(micButton_);
    langLabel_ = new QLabel(tr("Auto language"));
    langLabel_->setProperty("role", "muted");
    langLabel_->installEventFilter(this);
    barLayout_->addWidget(langLabel_);
    statusLabel_ = new QLabel(tr("Connecting…"));
    statusLabel_->setObjectName("serviceStatus");
    statusLabel_->setProperty("role", "caption");
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLabel_->installEventFilter(this);
    barLayout_->addWidget(statusLabel_, 1);
    expandButton_ = new QPushButton(tr("Preview"));
    expandButton_->setObjectName("previewButton");
    expandButton_->setProperty("variant", "ghost");
    expandButton_->setIcon(UiKit::icon("text-box"));
    expandButton_->setToolTip(tr("Open or collapse transcript preview"));
    connect(expandButton_, &QPushButton::clicked, this, [this]() {
        expanded_ ? showCompact() : showExpanded();
    });
    barLayout_->addWidget(expandButton_);

    auto *placement = new QMenu(placementButton_);
    placement->setObjectName("placementMenu");
    auto *group = new QActionGroup(placement);
    for (const auto &[id, label] : QList<QPair<QString, QString>>{
             {"free", tr("Free / draggable")}, {"top", tr("Top bar")}, {"bottom", tr("Bottom bar")},
             {"left", tr("Left rail")}, {"right", tr("Right rail")}}) {
        auto *action = placement->addAction(label);
        action->setData(id);
        action->setCheckable(true);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id]() { setDockPosition(id); });
    }
    connect(placement, &QMenu::aboutToShow, this, [this, group]() {
        for (auto *action : group->actions())
            action->setChecked(action->data().toString() == app_->settings()->dockPosition);
    });
    placementTimer_.setSingleShot(true);
    connect(&placementTimer_, &QTimer::timeout, this, [this, placement]() {
        placement->popup(placementButton_->mapToGlobal(QPoint(0, placementButton_->height())));
    });
    connect(placementButton_, &QPushButton::clicked, this, [this]() {
        placementTimer_.start(QApplication::doubleClickInterval());
    });
    settingsButton_ = new QPushButton;
    settingsButton_->setProperty("variant", "ghost");
    settingsButton_->setProperty("size", "icon");
    settingsButton_->setIcon(UiKit::icon("cog"));
    settingsButton_->setToolTip(tr("Settings"));
    settingsButton_->setAccessibleName(tr("Settings"));
    connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::openSettings);
    barLayout_->addWidget(settingsButton_);
    auto *hideButton = new QPushButton;
    hideButton->setProperty("variant", "ghost");
    hideButton->setProperty("size", "icon");
    hideButton->setIcon(UiKit::icon("close"));
    hideButton->setToolTip(tr("Hide to tray"));
    connect(hideButton, &QPushButton::clicked, this, &QWidget::hide);
    barLayout_->addWidget(hideButton);
    for (auto *button : {placementButton_, settingsButton_, hideButton}) {
        button->setFixedSize(36, 36);
        button->setIconSize(QSize(20, 20));
        button->setFocusPolicy(Qt::NoFocus);
    }
    expandButton_->setIconSize(QSize(20, 20));
    rootLayout_->addWidget(compactBar_);

    editorPage_ = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    editorPage_->setAttribute(Qt::WA_TranslucentBackground);
    editorPage_->setObjectName("previewWindow");
    auto *previewLayout = new QVBoxLayout(editorPage_);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    auto *editorShell = new QFrame;
    editorShell->setObjectName("editorPage");
    previewLayout->addWidget(editorShell);
    auto *editor = new QVBoxLayout(editorShell);
    editor->setContentsMargins(16, 16, 16, 10);
    editor->setSpacing(12);
    auto *title = new QLabel(tr("Transcript preview"));
    title->setProperty("role", "dialogTitle");
    auto *previewHeader = new QHBoxLayout;
    previewHeader->addWidget(title, 1);
    translateButton_ = new QPushButton(tr("Translate"));
    translateButton_->setObjectName("translateText");
    translateButton_->setProperty("variant", "ghost");
    translateButton_->setToolTip(tr("Translate reviewed text: Russian to English or English to Russian"));
    connect(translateButton_, &QPushButton::clicked, this, &MainWindow::translateResult);
    previewHeader->addWidget(translateButton_);
    autoTranslateBox_ = new QCheckBox(tr("Auto-translate"));
    autoTranslateBox_->setObjectName("autoTranslation");
    autoTranslateBox_->setChecked(app_->settings()->autoTranslate);
    connect(autoTranslateBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        app_->settings()->autoTranslate = enabled;
        app_->settings()->save();
        configureProcessing();
        if (userEditedResult_ && resultEdit_->toPlainText() != translation_) {
            processingLabel_->setText(tr("Your edited text was kept. Use Translate to translate this version."));
            return;
        }
        retryProcessing();
    });
    previewHeader->addWidget(autoTranslateBox_);
    editor->addLayout(previewHeader);
    comparison_ = new QSplitter(Qt::Horizontal);
    comparison_->setObjectName("comparison");
    comparison_->setChildrenCollapsible(false);
    const auto addPane = [this](const QString &title, const QString &name, bool readOnly) {
        auto *column = new QWidget;
        auto *layout = new QVBoxLayout(column);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        auto *label = new QLabel(title);
        label->setProperty("role", "muted");
        layout->addWidget(label);
        auto *edit = new QTextEdit;
        edit->setObjectName(name);
        edit->setReadOnly(readOnly);
        edit->setAcceptRichText(false);
        edit->setMinimumWidth(0);
        edit->setMinimumHeight(200);
        edit->setLineWrapMode(QTextEdit::WidgetWidth);
        edit->viewport()->installEventFilter(this);
        layout->addWidget(edit, 1);
        comparison_->addWidget(column);
        if (!readOnly) resultLabel_ = label;
        return edit;
    };
    originalEdit_ = addPane(tr("Original"), "originalText", true);
    originalEdit_->setPlaceholderText(tr("Live transcription appears here while you speak."));
    resultEdit_ = addPane(tr("Reviewed text"), "resultText", false);
    resultEdit_->setPlaceholderText(tr("Corrections appear here. Review or edit before inserting."));
    comparison_->setStretchFactor(0, 1);
    comparison_->setStretchFactor(1, 1);
    comparison_->setSizes({1000, 1000});
    editor->addWidget(comparison_, 1);
    const auto linkScroll = [](QScrollBar *from, QScrollBar *to) {
        QObject::connect(from, &QScrollBar::valueChanged, to, [from, to](int value) {
            const QSignalBlocker blocker(to);
            to->setValue(from->maximum() ? qRound(double(value) * to->maximum() / from->maximum()) : 0);
        });
    };
    linkScroll(originalEdit_->verticalScrollBar(), resultEdit_->verticalScrollBar());
    linkScroll(resultEdit_->verticalScrollBar(), originalEdit_->verticalScrollBar());
    connect(resultEdit_, &QTextEdit::textChanged, this, [this]() {
        userEditedResult_ = true;
        reviewedConfirmed_ = true;
        corrections_.clear();
        showCorrections();
        updateActions();
        saveSessionRecord();
    });
    for (auto *edit : {originalEdit_, resultEdit_}) {
        edit->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(edit, &QTextEdit::customContextMenuRequested, this, [this, edit](const QPoint &point) {
            QMenu *menu = edit->createStandardContextMenu();
            menu->addSeparator();
            auto *retry = menu->addAction(tr("Retry correction"), this, &MainWindow::retryProcessing);
            retry->setEnabled(!recording_ && !finishing_ && !originalSoFar_.isEmpty());
            if (edit == resultEdit_) {
                auto *translate = menu->addAction(tr("Translate RU / EN"), this, &MainWindow::translateResult);
                translate->setEnabled(!recording_ && !finishing_ && !edit->toPlainText().trimmed().isEmpty());
            }
            auto *cancel = menu->addAction(tr("Cancel recording"), this, &MainWindow::cancelDictation);
            cancel->setEnabled(recording_ || finishing_);
            menu->exec(edit->viewport()->mapToGlobal(point));
            delete menu;
        });
    }
    reviewBar_ = new QWidget;
    auto *review = new QHBoxLayout(reviewBar_);
    review->setContentsMargins(0, 0, 0, 0);
    changeLabel_ = new QLabel;
    changeLabel_->setWordWrap(true);
    changeLabel_->setProperty("role", "caption");
    review->addWidget(changeLabel_, 1);
    acceptButton_ = new QPushButton(tr("Accept"));
    acceptButton_->setObjectName("acceptCorrection");
    rejectButton_ = new QPushButton(tr("Reject"));
    rejectButton_->setObjectName("rejectCorrection");
    acceptButton_->setProperty("variant", "outline");
    rejectButton_->setProperty("variant", "ghost");
    acceptButton_->setToolTip(tr("Accept all current corrections"));
    rejectButton_->setToolTip(tr("Reject all current corrections"));
    connect(acceptButton_, &QPushButton::clicked, this, [this]() { reviewCorrections(true); });
    connect(rejectButton_, &QPushButton::clicked, this, [this]() { reviewCorrections(false); });
    review->addWidget(acceptButton_);
    review->addWidget(rejectButton_);
    editor->addWidget(reviewBar_);
    processingLabel_ = new QLabel(tr("Language and context apply automatically. Speak to begin."));
    processingLabel_->setObjectName("processingStatus");
    processingLabel_->setWordWrap(true);
    processingLabel_->setProperty("role", "caption");
    editor->addWidget(processingLabel_);
    auto *actions = new QHBoxLayout;
    saveButton_ = new QPushButton(tr("Save"));
    saveButton_->setObjectName("saveReviewedText");
    saveButton_->setProperty("variant", "ghost");
    saveButton_->setIcon(UiKit::icon("folder"));
    connect(saveButton_, &QPushButton::clicked, this, &MainWindow::saveReviewedText);
    actions->addWidget(saveButton_);
    historyButton_ = new QPushButton(tr("History"));
    historyButton_->setObjectName("transcriptHistory");
    historyButton_->setProperty("variant", "ghost");
    historyButton_->setToolTip(tr("Restore a previous transcript, including text that could not be inserted or saved"));
    connect(historyButton_, &QPushButton::clicked, this, [this]() {
        QMenu menu(historyButton_);
        for (int i = preservedTranscripts_.size() - 1; i >= 0; --i) {
            const auto &record = preservedTranscripts_[i].record;
            auto *action = menu.addAction(record.started.toString("HH:mm:ss") + " · " + record.original.left(48));
            connect(action, &QAction::triggered, this, [this, i]() { restoreTranscript(i); });
        }
        menu.exec(historyButton_->mapToGlobal(QPoint(0, historyButton_->height())));
    });
    actions->addWidget(historyButton_);
    actions->addStretch();
    copyButton_ = new QPushButton(tr("Copy"));
    copyButton_->setProperty("variant", "outline");
    connect(copyButton_, &QPushButton::clicked, this, &MainWindow::copyResult);
    actions->addWidget(copyButton_);
    insertButton_ = new QPushButton(tr("Insert"));
    insertButton_->setProperty("variant", "primary");
    insertButton_->setToolTip(tr("Insert into the remembered field without submitting."));
    connect(insertButton_, &QPushButton::clicked, this, [this]() { insertResult(); });
    actions->addWidget(insertButton_);
    actions->addWidget(new QSizeGrip(editorPage_));
    editor->addLayout(actions);
    setCentralWidget(root);
    editorPage_->hide();
    reviewBar_->hide();
    resize(640, 56);
    updateMicrophone();
}

void MainWindow::configureProcessing() {
    app_->processor()->setMode(app_->settings()->autoCleanEditor ? "auto_correct" : "dictation");
    app_->processor()->setAutoTranslate(app_->settings()->autoTranslate);
    app_->processor()->setProfile("general");
    app_->service()->setContext(app_->settings()->context);
}

void MainWindow::startNewTranscript() {
    if (!recording_ || finishing_ || speechActive_) return;
    if (originalSoFar_.trimmed().isEmpty() && processedSoFar_.trimmed().isEmpty()) return;
    // Review keeps the whole recording for confirmation. Quick capture must
    // continue even when the destination or optional storage is unavailable.
    if (reviewMode_) return;
    if (app_->dictation()->pending() || app_->processor()->busy() ||
        !previews_.isEmpty() || insertTimer_.isActive()) {
        transcriptTimer_.start(250);
        return;
    }
    if (!app_->processor()->pendingOriginal().isEmpty()) {
        app_->processor()->finish();
        if (app_->processor()->busy()) { transcriptTimer_.start(250); return; }
        scheduleQuickInsert();
        if (insertTimer_.isActive()) { transcriptTimer_.start(250); return; }
    }
    preserveTranscript();
    app_->processor()->reset();
    insertedEarlierDocument_ |= !insertedText_.isEmpty();
    insertionFailed_ = false;
    insertedText_.clear();
    finalFragment_.clear();
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sessionStartedAt_ = QDateTime::currentDateTime();
    sessionEndedAt_ = {};
    originalSoFar_.clear();
    processedSoFar_.clear();
    fragmentText_.clear();
    previews_.clear();
    translation_.clear();
    translationLanguage_.clear();
    acceptedChanges_.clear();
    corrections_.clear();
    currentCorrection_ = 0;
    userEditedResult_ = false;
    reviewedConfirmed_ = false;
    updateOriginal();
    updateResult();
    statusLabel_->setText(tr("New transcript after %1 s of silence").arg(app_->settings()->transcriptResetS));
}

void MainWindow::updateOriginal() {
    const int reset = app_->settings()->transcriptResetS;
    if (recording_ && !speechActive_ && reset > 0 && !originalSoFar_.trimmed().isEmpty() && !transcriptTimer_.isActive())
        transcriptTimer_.start(reset * 1000);
    else if (!recording_) transcriptTimer_.stop();
    originalEdit_->clear();
    QTextCursor cursor(originalEdit_->document());
    cursor.insertText(originalSoFar_);
    QTextCharFormat provisional;
    provisional.setForeground(QColor("#c9bd9a"));
    provisional.setFontItalic(true);
    for (const QString &text : previews_) {
        if (!text.isEmpty()) cursor.insertText((cursor.position() ? " " : "") + text, provisional);
    }
    showCorrections();
}

void MainWindow::toggleDictation(bool reviewMode) {
    if (finishing_) return;
    if (recording_) stopDictation();
    else startDictation(reviewMode);
}

void MainWindow::startDictation(bool reviewMode) {
    if (recording_ || finishing_) return;
    preserveTranscript();
    ++generation_;
    releaseFocusTarget(target_);
    target_ = captureCurrentFocus();
    if (target_.windowHandle == quintptr(winId()) || target_.windowHandle == quintptr(editorPage_->winId()))
        releaseFocusTarget(target_);
    reviewMode_ = reviewMode;
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sessionStartedAt_ = QDateTime::currentDateTime();
    sessionEndedAt_ = {};
    processingDone_ = false;
    insertionFailed_ = speechActive_ = reviewedConfirmed_ = false;
    insertedEarlierDocument_ = false;
    insertedText_.clear();
    finalFragment_.clear();
    insertTimer_.stop();
    transcriptTimer_.stop();
    originalSoFar_.clear();
    processedSoFar_.clear();
    fragmentText_.clear();
    translation_.clear();
    translationLanguage_.clear();
    previews_.clear();
    userEditedResult_ = false;
    acceptedChanges_.clear();
    app_->processor()->reset();
    app_->settings()->loadContext();
    configureProcessing();
    resultLabel_->setText(tr("Reviewed text"));
    updateOriginal();
    updateResult();
    auto *settings = app_->settings();
    DictationParams params;
    params.speechThreshold = settings->speechThreshold;
    params.silenceFrames = settings->silenceFrames;
    params.minSpeechFrames = settings->minSpeechFrames;
    params.prerollFrames = settings->prerollFrames;
    params.maxUtteranceS = settings->maxUtteranceS;
    params.speculateEveryFrames = settings->speculateEveryFrames;
    params.speculative = settings->speculative;
    params.asrModel = settings->asrModel;
    params.language = "auto";
    app_->dictation()->setParams(params);
    app_->dictation()->setInputDevice(settings->inputDevice);
    if (!app_->dictation()->start()) { updateActions(); return; }
    recording_ = sessionActive_ = true;
    statusLabel_->setText(tr("Listening…"));
    processingLabel_->setText(target_.isValid() ? tr("Target: %1").arg(target_.windowTitle)
                                               : tr("No target selected. Use Copy when ready."));
    updateActions();
}

void MainWindow::stopDictation() {
    if (!recording_) return;
    recording_ = false;
    finishing_ = true;
    transcriptTimer_.stop();
    sessionEndedAt_ = QDateTime::currentDateTime();
    statusLabel_->setText(tr("Finishing…"));
    updateMicrophone();
    updateActions();
    app_->dictation()->stop();
}

void MainWindow::cancelDictation() {
    ++generation_;
    recording_ = finishing_ = sessionActive_ = false;
    insertTimer_.stop();
    transcriptTimer_.stop();
    speechActive_ = processingDone_ = false;
    app_->dictation()->cancel();
    app_->processor()->reset();
    previews_.clear();
    acceptedChanges_.clear();
    updateOriginal();
    statusLabel_->setText(tr("Cancelled"));
    updateMicrophone();
    updateActions();
}

void MainWindow::onFinalTranscript(const QString &text, qreal seconds, int id) {
    Q_UNUSED(seconds);
    previews_.remove(id);
    if (text.trimmed().isEmpty()) return;
    originalSoFar_ += (originalSoFar_.isEmpty() ? "" : " ") + text;
    updateOriginal();
    app_->processor()->acceptFinal(text);
    saveSessionRecord();
}

void MainWindow::onProcessedBatch(const QString &original, const QString &result) {
    Q_UNUSED(original);
    processedSoFar_ += (processedSoFar_.isEmpty() ? "" : " ") + result;
    updateResult();
    scheduleQuickInsert();
    saveSessionRecord();
}

void MainWindow::updateResult() {
    if (!userEditedResult_) {
        const QSignalBlocker blocker(resultEdit_);
        const QString result = (processedSoFar_ + " " + fragmentText_).trimmed();
        resultEdit_->setPlainText(result);
        // Translation rewrites the wording of the whole text. Diffing it against
        // the transcript would mark nearly every word, so the translated pane is
        // shown as a result: no correction highlights, no Accept/Reject.
        if (app_->settings()->autoTranslate) {
            translation_ = result;
            translationLanguage_ = originalSoFar_.contains(QRegularExpression(QStringLiteral("[\\x{0400}-\\x{04ff}]"))) ? "en" : "ru";
            resultLabel_->setText(tr("Translation · RU ↔ EN"));
            corrections_.clear();
        } else {
            resultLabel_->setText(tr("Reviewed text"));
            corrections_ = pendingChanges(originalSoFar_, resultEdit_->toPlainText());
        }
        currentCorrection_ = qBound(0, currentCorrection_, qMax(0, int(corrections_.size()) - 1));
        showCorrections();
    }
    updateActions();
}

QVector<TextChange> MainWindow::pendingChanges(const QString &original, const QString &result) const {
    QVector<TextChange> changes = textChanges(original, result);
    if (acceptedChanges_.isEmpty()) return changes;
    QVector<TextChange> pending;
    pending.reserve(changes.size());
    for (const auto &change : changes) {
        const auto decision = qMakePair(original.mid(change.originalStart, change.originalLength),
                                        result.mid(change.resultStart, change.resultLength));
        if (!acceptedChanges_.contains(decision)) pending.append(change);
    }
    return pending;
}

void MainWindow::showCorrections() {
    QList<QTextEdit::ExtraSelection> originals, results;
    for (int i = 0; i < corrections_.size(); ++i) {
        const auto &change = corrections_[i];
        const auto selection = [i, this](QTextEdit *edit, int start, int length, const QColor &color) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(edit->document());
            const int limit = qMax(0, edit->document()->characterCount() - 1);
            mark.cursor.setPosition(qMin(start, limit));
            mark.cursor.setPosition(qMin(start + qMax(1, length), limit), QTextCursor::KeepAnchor);
            mark.format.setBackground(color);
            if (i == currentCorrection_) mark.format.setFontUnderline(true);
            return mark;
        };
        originals.append(selection(originalEdit_, change.originalStart, change.originalLength, QColor(229, 84, 78, 65)));
        results.append(selection(resultEdit_, change.resultStart, change.resultLength, QColor(238, 154, 58, 65)));
    }
    originalEdit_->setExtraSelections(originals);
    resultEdit_->setExtraSelections(results);
    reviewBar_->setVisible(!corrections_.isEmpty());
    changeLabel_->setText(tr("%1 corrections · Accept or Reject all").arg(corrections_.size()));
}

void MainWindow::reviewCorrections(bool accept) {
    if (finishing_) return;
    const int count = corrections_.size();
    for (int i = 0; i < count && !corrections_.isEmpty(); ++i) {
        // Reject from the end so earlier offsets stay valid in restored text.
        currentCorrection_ = accept ? 0 : qMin(count - i - 1, int(corrections_.size()) - 1);
        reviewCorrection(accept);
    }
    saveSessionRecord(true);
}

void MainWindow::reviewCorrection(bool accept) {
    if (finishing_ || corrections_.isEmpty()) return;
    const auto change = corrections_.at(currentCorrection_);
    const QString shown = resultEdit_->toPlainText();
    const QString originalText = originalSoFar_.mid(change.originalStart, change.originalLength);
    const QString correctedText = shown.mid(change.resultStart, change.resultLength);
    if (accept) {
        acceptedChanges_.append(qMakePair(originalText, correctedText));
        corrections_.removeAt(currentCorrection_);
        currentCorrection_ = qMin(currentCorrection_, qMax(0, int(corrections_.size()) - 1));
        showCorrections();
        updateActions();
        return;
    }
    // Reject rewrites the corrected wording inside the processed text, so the
    // decision survives the next batch instead of freezing the whole pane.
    if (!userEditedResult_ && change.resultStart >= 0 && change.resultStart + change.resultLength <= processedSoFar_.size()) {
        processedSoFar_.replace(change.resultStart, change.resultLength, originalText);
        updateResult();
        return;
    }
    // A change that reaches into the pending tail cannot be folded back into
    // processed text: apply it in place and stop following later results.
    {
        const QSignalBlocker blocker(resultEdit_);
        QTextCursor cursor(resultEdit_->document());
        cursor.setPosition(change.resultStart);
        cursor.setPosition(change.resultStart + change.resultLength, QTextCursor::KeepAnchor);
        cursor.insertText(originalText);
    }
    userEditedResult_ = true;
    corrections_.removeAt(currentCorrection_);
    currentCorrection_ = qMin(currentCorrection_, qMax(0, int(corrections_.size()) - 1));
    showCorrections();
    updateActions();
}

void MainWindow::retryProcessing() {
    if (originalSoFar_.trimmed().isEmpty()) return;
    ++generation_;
    app_->processor()->reset();
    configureProcessing();
    processedSoFar_.clear();
    fragmentText_.clear();
    translation_.clear();
    translationLanguage_.clear();
    userEditedResult_ = false;
    acceptedChanges_.clear();
    finishing_ = !recording_;
    resultLabel_->setText(tr("Reviewed text"));
    updateResult();
    app_->processor()->acceptFinal(originalSoFar_);
    if (!recording_) app_->processor()->finish();
}

void MainWindow::translateResult() {
    const QString text = resultEdit_->toPlainText();
    if (recording_ || finishing_ || text.trimmed().isEmpty()) return;
    static const QRegularExpression cyrillic(QStringLiteral("[\\x{0400}-\\x{04ff}]"));
    const bool fromRussian = text.contains(cyrillic);
    const QString mode = fromRussian ? "translate_ru_en" : "translate_en_ru";
    const int revision = resultEdit_->document()->revision();
    const quint64 generation = ++generation_;
    finishing_ = true;
    updateActions();
    QPointer<MainWindow> self(this);
    app_->service()->processText(text, mode, "general", {}, {}, [this, self, generation, revision, fromRussian]
        (bool ok, QString translated, QString, QString error) {
        if (!self || generation != generation_) return;
        finishing_ = false;
        if (!ok) processingLabel_->setText(error);
        else if (revision != resultEdit_->document()->revision())
            processingLabel_->setText(tr("Your edit was kept. Translate again when ready."));
        else {
            const QSignalBlocker blocker(resultEdit_);
            translation_ = translated;
            translationLanguage_ = fromRussian ? "en" : "ru";
            resultEdit_->setPlainText(translated);
            resultLabel_->setText(fromRussian ? tr("English translation") : tr("Russian translation"));
            userEditedResult_ = true;
            acceptedChanges_.clear();
            corrections_.clear();
            showCorrections();
        }
        updateActions();
        saveSessionRecord();
    });
}

void MainWindow::updateActions() {
    const bool hasText = !resultEdit_->toPlainText().trimmed().isEmpty();
    micButton_->setEnabled(!finishing_);
    micButton_->setToolTip(recording_ ? tr("Stop recording") : tr("Start recording · %1").arg(app_->settings()->insertHotkey));
    // Settings stays reachable while a correction is in flight: a stuck request
    // must not lock the user out of the panel.
    settingsButton_->setEnabled(!recording_);
    copyButton_->setEnabled(hasText);
    saveButton_->setEnabled(hasText && !recording_ && !finishing_);
    saveButton_->setToolTip(tr("Save reviewed text · %1").arg(app_->settings()->saveHotkey));
    historyButton_->setEnabled(!preservedTranscripts_.isEmpty() && !recording_ && !finishing_ && !insertTimer_.isActive());
    insertButton_->setEnabled(hasText && target_.isValid() && !recording_ && !finishing_);
    translateButton_->setEnabled(hasText && !recording_ && !finishing_);
    // Decisions belong to completed corrections, not to the microphone: they
    // stay available while dictation continues. Only a draining session or a
    // translation in flight makes the reviewed text change under the cursor.
    acceptButton_->setEnabled(!finishing_ && !corrections_.isEmpty());
    rejectButton_->setEnabled(!finishing_ && !corrections_.isEmpty());
}

void MainWindow::updateMicrophone(double rms) {
    const double level = recording_ ? qBound(0.0, (20.0 * std::log10(qMax(rms, 1e-6)) + 60) / 60, 1.0) : 0;
    microphoneLevel_ = recording_ ? microphoneLevel_ * 0.5 + level * 0.5 : 0;
    micButton_->setIcon(UiKit::microphoneIcon(microphoneLevel_, recording_));
}

void MainWindow::finishSession() {
    if (!finishing_) return;
    processingDone_ = true;
    scheduleQuickInsert();
    if (insertTimer_.isActive()) return;
    finishing_ = false;
    statusLabel_->setText(tr("Ready"));
    updateActions();
    if (!sessionActive_) return;
    sessionActive_ = false;
    saveSessionRecord();
    if (reviewMode_ || insertionFailed_ || userEditedResult_ || !app_->processor()->pendingOriginal().isEmpty()) showExpanded();
}

void MainWindow::scheduleQuickInsert() {
    if (reviewMode_ || !sessionActive_ || !target_.isValid() || userEditedResult_ || insertionFailed_ || insertTimer_.isActive()) return;
    const QString ready = (processedSoFar_ + " " + finalFragment_).trimmed();
    if (ready != insertedText_ && !ready.isEmpty()) insertTimer_.start(0);
}

void MainWindow::insertResult(bool automatic) {
    if (automatic && (reviewMode_ || !sessionActive_ || userEditedResult_ || insertionFailed_)) return;
    if (!automatic && (recording_ || finishing_ || insertTimer_.isActive())) return;
    const QString text = automatic ? (processedSoFar_ + " " + finalFragment_).trimmed() : resultEdit_->toPlainText();
    if (text.trimmed().isEmpty() || text == insertedText_) return;
    if (!text.startsWith(insertedText_)) {
        insertionFailed_ = true;
        processingLabel_->setText(tr("Previously inserted text was edited. Copy the changed text to avoid inserting it twice."));
        showExpanded();
        return;
    }
    QString remaining = text.mid(insertedText_.size());
    if (insertedText_.isEmpty() && insertedEarlierDocument_) remaining.prepend(' ');
    const auto report = insertIntoTarget(target_, remaining);
    if (report.result == InsertResult::Ok || report.result == InsertResult::ClipboardFallback) {
        insertedText_ = text;
        insertionFailed_ = false;
        processingLabel_->setText(tr("Inserted without submitting."));
        saveSessionRecord(true);
        // Clipboard fallback restores after one second. Serialize the next batch
        // and keep finalization guarded until that paste has been delivered.
        if (automatic) insertTimer_.start(report.result == InsertResult::ClipboardFallback ? 1100 : 0);
    } else {
        insertionFailed_ = true;
        processingLabel_->setText(tr("Text kept. Insertion unavailable: %1. Recording can continue.").arg(report.detail));
        if (!automatic) showExpanded();
    }
}

void MainWindow::copyResult() {
    QApplication::clipboard()->setText(resultEdit_->toPlainText());
    processingLabel_->setText(QApplication::clipboard()->text() == resultEdit_->toPlainText()
        ? tr("Copied.") : tr("Clipboard is unavailable. The text is kept for another attempt."));
}

SessionRecord MainWindow::sessionRecord(bool confirmed) {
    if (sessionId_.isEmpty()) sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!sessionStartedAt_.isValid()) sessionStartedAt_ = QDateTime::currentDateTime();
    SessionRecord record;
    record.sessionId = sessionId_;
    record.mode = app_->settings()->autoCleanEditor ? "auto_correct" : "dictation";
    record.sourceLang = "auto";
    record.original = originalSoFar_;
    record.corrected = processedSoFar_;
    record.translation = translation_;
    record.targetLang = translationLanguage_;
    if (confirmed || reviewedConfirmed_) record.finalText = resultEdit_->toPlainText();
    record.started = sessionStartedAt_;
    record.ended = sessionEndedAt_.isValid() ? sessionEndedAt_ : QDateTime::currentDateTime();
    return record;
}

bool MainWindow::saveSessionRecord(bool confirmed, bool force) {
    reviewedConfirmed_ |= confirmed;
    auto *store = app_->store();
    if (!store->enabled() || (!force && !app_->settings()->autoSaveSessions)) return false;
    if (originalSoFar_.isEmpty() && resultEdit_->toPlainText().isEmpty()) return false;
    const auto record = sessionRecord(confirmed);
    QString error;
    if (!store->saveRecord(record, &error)) { processingLabel_->setText(error); return false; }
    return true;
}

void MainWindow::preserveTranscript() {
    if (originalSoFar_.isEmpty() && resultEdit_->toPlainText().isEmpty()) return;
    saveSessionRecord();
    preservedTranscripts_.append({sessionRecord(false), resultEdit_->toPlainText(), insertedText_,
        retainFocusTarget(target_), acceptedChanges_, corrections_, insertionFailed_, reviewedConfirmed_});
}

void MainWindow::restoreTranscript(int index) {
    if (recording_ || finishing_ || insertTimer_.isActive() || index < 0 || index >= preservedTranscripts_.size()) return;
    const auto saved = preservedTranscripts_.takeAt(index);
    preserveTranscript();
    releaseFocusTarget(target_);
    target_ = saved.target;
    ++generation_;
    transcriptTimer_.stop();
    app_->processor()->reset();
    sessionActive_ = false;
    reviewMode_ = true;
    sessionId_ = saved.record.sessionId;
    sessionStartedAt_ = saved.record.started;
    sessionEndedAt_ = saved.record.ended;
    originalSoFar_ = saved.record.original;
    processedSoFar_ = saved.record.corrected;
    translation_ = saved.record.translation;
    translationLanguage_ = saved.record.targetLang;
    insertedText_ = saved.insertedText;
    fragmentText_.clear();
    finalFragment_.clear();
    previews_.clear();
    insertionFailed_ = saved.insertionFailed;
    insertedEarlierDocument_ = false;
    reviewedConfirmed_ = saved.confirmed;
    acceptedChanges_ = saved.acceptedChanges;
    corrections_ = saved.corrections;
    userEditedResult_ = true;
    { const QSignalBlocker blocker(resultEdit_); resultEdit_->setPlainText(saved.reviewedText); }
    resultLabel_->setText(translation_.isEmpty() ? tr("Reviewed text") : tr("Translation · RU ↔ EN"));
    updateOriginal();
    updateActions();
    showExpanded();
}

void MainWindow::saveReviewedText() {
    if (recording_ || finishing_ || resultEdit_->toPlainText().trimmed().isEmpty()) return;
    if (!app_->store()->enabled()) {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder for reviewed transcripts"));
        if (folder.isEmpty()) return;
        app_->settings()->markdownDir = folder;
        app_->settings()->save();
        app_->store()->setDir(folder);
    }
    if (sessionId_.isEmpty()) {
        sessionId_ = QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
        sessionStartedAt_ = QDateTime::currentDateTime();
    }
    if (saveSessionRecord(true, true)) processingLabel_->setText(tr("Saved to %1").arg(app_->store()->dir()));
}

void MainWindow::updateDockLayout() {
    const QString dock = app_->settings()->dockPosition;
    const bool vertical = dock == "left" || dock == "right";
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    compactBar_->setMinimumSize(0, 0);
    compactBar_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    barLayout_->setDirection(vertical ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    barLayout_->setContentsMargins(vertical ? 8 : 10, 8, vertical ? 8 : 10, 8);
    langLabel_->setVisible(!vertical);
    statusLabel_->setVisible(!vertical);
    expandButton_->setText(vertical ? QString() : tr("Preview"));
    expandButton_->setMinimumSize(0, 0);
    expandButton_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    expandButton_->setProperty("size", vertical ? "icon" : "default");
    expandButton_->style()->unpolish(expandButton_);
    expandButton_->style()->polish(expandButton_);
    if (vertical) {
        compactBar_->setFixedSize(52, 228);
        expandButton_->setFixedSize(36, 36);
        setFixedSize(56, 232);
    } else {
        compactBar_->setFixedHeight(52);
        setFixedSize(640, 56);
    }
}

void MainWindow::showCompact() {
    show();
    if (!expanded_) return;
    app_->settings()->editorGeometry = editorPage_->saveGeometry();
    expanded_ = false;
    editorPage_->hide();
    saveGeometrySettings();
}

void MainWindow::showExpanded() {
    if (!expanded_) {
        expanded_ = true;
        if (!app_->settings()->editorGeometry.isEmpty()) editorPage_->restoreGeometry(app_->settings()->editorGeometry);
        else editorPage_->resize(704, 504);
    }
    show();
    positionPreview();
    editorPage_->show();
    editorPage_->raise();
    raise();
}

void MainWindow::setDockPosition(const QString &position) {
    app_->settings()->dockPosition = position;
    changingGeometry_ = true;
    applyDockPosition();
    changingGeometry_ = false;
    saveGeometrySettings();
}

void MainWindow::applyDockPosition() {
    updateDockLayout();
    QScreen *display = QGuiApplication::screenAt(frameGeometry().center());
    if (!display) display = screen();
    const QRect area = display->availableGeometry().adjusted(8, 8, -8, -8);
    QPoint point(qBound(area.left(), x(), qMax(area.left(), area.right() - width() + 1)),
                 qBound(area.top(), y(), qMax(area.top(), area.bottom() - height() + 1)));
    const QString dock = app_->settings()->dockPosition;
    if (dock == "top" || dock == "bottom") {
        point.setX(area.center().x() - rect().center().x());
        point.setY(dock == "top" ? area.top() : area.bottom() - height() + 1);
    }
    if (dock == "left" || dock == "right") {
        point.setX(dock == "left" ? area.left() : area.right() - width() + 1);
        point.setY(area.center().y() - rect().center().y());
    }
    move(point);
    positionPreview();
}

void MainWindow::positionPreview() {
    if (!expanded_) return;
    const QRect area = screen()->availableGeometry().adjusted(8, 8, -8, -8);
    const QString dock = app_->settings()->dockPosition;
    const bool vertical = dock == "left" || dock == "right";
    QPoint point;
    editorPage_->setMinimumWidth(0);
    editorPage_->setMaximumWidth(QWIDGETSIZE_MAX);
    if (vertical) {
        const int available = dock == "left" ? area.right() - geometry().right() : x() - area.left();
        editorPage_->resize(qMin(qMax(640, editorPage_->width()), available), qMin(qMax(360, editorPage_->height()), area.height()));
        point = QPoint(dock == "left" ? geometry().right() + 1 : x() - editorPage_->width(),
                       qBound(area.top(), geometry().center().y() - editorPage_->rect().center().y(), area.bottom() - editorPage_->height() + 1));
    } else {
        editorPage_->setFixedWidth(width());
        const int below = area.bottom() - geometry().bottom();
        const int above = y() - area.top();
        const bool upwards = dock == "bottom" || (dock == "free" && below < 360 && above > below);
        editorPage_->resize(width(), qMin(qMax(360, editorPage_->height()), upwards ? above : below));
        point = QPoint(x(), upwards ? y() - editorPage_->height() : geometry().bottom() + 1);
    }
    editorPage_->move(point);
}

QString MainWindow::nearestEdge() const {
    const QRect area = screen()->availableGeometry();
    const QList<QPair<int, QString>> edges{
        {qAbs(y() - area.top()), "top"}, {qAbs(geometry().bottom() - area.bottom()), "bottom"},
        {qAbs(x() - area.left()), "left"}, {qAbs(geometry().right() - area.right()), "right"}};
    auto nearest = edges.first();
    for (const auto &edge : edges) if (edge.first < nearest.first) nearest = edge;
    return nearest.second;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (originalEdit_ && resultEdit_ && (watched == originalEdit_->viewport() || watched == resultEdit_->viewport())) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *edit = watched == originalEdit_->viewport() ? originalEdit_ : resultEdit_;
            const int position = edit->cursorForPosition(static_cast<QMouseEvent *>(event)->position().toPoint()).position();
            for (int i = 0; i < corrections_.size(); ++i) {
                const auto &change = corrections_[i];
                const int start = edit == originalEdit_ ? change.originalStart : change.resultStart;
                const int length = edit == originalEdit_ ? change.originalLength : change.resultLength;
                if (position >= start && position <= start + qMax(1, length)) { currentCorrection_ = i; showCorrections(); break; }
            }
        }
        return false;
    }
    if (watched == placementButton_ && event->type() == QEvent::MouseButtonDblClick) {
        placementTimer_.stop();
        dragArmed_ = dragging_ = false;
        placementButton_->setDown(false);
        setDockPosition(nearestEdge());
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            placementTimer_.stop();
            dragArmed_ = true;
            dragStart_ = mouse->globalPosition().toPoint();
            dragOffset_ = mouse->globalPosition().toPoint() - pos();
            return watched != placementButton_;
        }
    } else if (event->type() == QEvent::MouseMove && dragArmed_) {
        const QPoint point = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
        if (!dragging_ && (point - dragStart_).manhattanLength() < QApplication::startDragDistance()) return false;
        dragging_ = true;
        move(point - dragOffset_);
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && dragArmed_) {
        dragArmed_ = false;
        if (!dragging_) return watched != placementButton_;
        dragging_ = false;
        placementButton_->setDown(false);
        const QRect area = screen()->availableGeometry();
        const bool nearEdge = qMin(qMin(qAbs(y() - area.top()), qAbs(geometry().bottom() - area.bottom())),
                               qMin(qAbs(x() - area.left()), qAbs(geometry().right() - area.right()))) < 25;
        setDockPosition(nearEdge ? nearestEdge() : QString("free"));
        return true;
    }
    return false;
}

bool MainWindow::nativeEvent(const QByteArray &type, void *message, qintptr *result) {
    const auto *msg = static_cast<MSG *>(message);
    if (type == "windows_generic_MSG" && msg->message == WM_MOUSEACTIVATE) {
        *result = MA_NOACTIVATE;
        return true;
    }
    return QMainWindow::nativeEvent(type, message, result);
}

void MainWindow::updateServiceStatus() {
    QPointer<MainWindow> self(this);
    app_->service()->getJson("/health", [self](bool ok, QByteArray body, QString) {
        if (!self || self->recording_ || self->finishing_) return;
        const bool ready = ok && QJsonDocument::fromJson(body).object().value("status").toString() == "ok";
        self->statusLabel_->setText(ready ? self->tr("Ready") : self->tr("Service offline"));
        self->micButton_->setAccessibleDescription(ready ? self->tr("Local service connected") : self->tr("Local service offline"));
    });
}

void MainWindow::saveGeometrySettings() {
    if (changingGeometry_ || dragging_) return;
    app_->settings()->compactGeometry = saveGeometry();
    if (expanded_) app_->settings()->editorGeometry = editorPage_->saveGeometry();
    app_->settings()->save();
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    positionPreview();
    saveGeometrySettings();
}

void MainWindow::moveEvent(QMoveEvent *event) {
    QMainWindow::moveEvent(event);
    positionPreview();
    saveGeometrySettings();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveGeometrySettings();
    hide();
    event->ignore();
}

void MainWindow::hideEvent(QHideEvent *event) {
    if (editorPage_) editorPage_->hide();
    QMainWindow::hideEvent(event);
}

void MainWindow::openSettings() {
    SettingsDialog dialog(app_, this);
    if (dialog.exec() == QDialog::Accepted) {
        configureProcessing();
        changingGeometry_ = true;
        applyDockPosition();
        changingGeometry_ = false;
        updateActions();
        updateServiceStatus();
    }
}

void MainWindow::openVoicePlayback() {
    VoicePlaybackDialog dialog(app_, this);
    dialog.exec();
}
