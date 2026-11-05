#include "app/MeetingWindow.h"
#include "app/PanelApp.h"
#include "app/SettingsPopover.h"
#include "app/UiKit.h"
#include "audio/WasapiCapture.h"
#include "core/AudioMeter.h"
#include "core/LevelBadge.h"
#include "core/SessionController.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextEdit>
#include <QInputDialog>
#include <QMenu>
#include <QTextStream>
#include <QVBoxLayout>

namespace VoicePanel {

namespace {
QString clock(double seconds) {
    const int total = int(seconds);
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}
QString fileNameStamp(const QDateTime &at) {
    // Windows-safe (no colons), sorts by date, carries the full start time.
    return at.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
}
} // namespace

MeetingWindow::MeetingWindow(PanelApp *app, QWidget *parent) : QDialog(parent), app_(app) {
    setObjectName(QStringLiteral("voicePlayback"));   // shared dialog surface
    setWindowIcon(UiKit::icon("users"));
    setWindowTitle(tr("VoicePanel — meeting transcript"));
    resize(720, 560);
    controller_ = new SessionController(app_->service(), app_->tts(), this);
    buildUi();
    buildSettings();
    refreshDevices();

    connect(controller_, &SessionController::line, this,
            [this](const QString &speaker, const QString &text, double at) {
        appendLine(speaker, text, at);
    });
    transcript_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(transcript_, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
        QMenu menu(this);
        QStringList speakers;
        for (const Line &line : lines_) {
            if (!speakers.contains(line.speaker)) speakers << line.speaker;
        }
        for (const QString &speaker : speakers) {
            const QString display = names_.value(speaker, speaker);
            menu.addAction(tr("Rename “%1”…").arg(display), this,
                           [this, speaker] { renameSpeaker(speaker); });
        }
        if (!speakers.isEmpty()) menu.addSeparator();
        menu.addAction(tr("Copy transcript"), this, [this] {
            transcript_->selectAll();
            transcript_->copy();
        });
        menu.exec(transcript_->mapToGlobal(at));
    });
    connect(controller_, &SessionController::level, this,
            [this](double rms) {
                micBadge_->setRecording(true);
                micBadge_->setLevel(AudioMeter::normalized(rms));
            });
    connect(controller_, &SessionController::levelRemote, this,
            [this](double rms) {
                remoteBadge_->setRecording(true);
                remoteBadge_->setLevel(AudioMeter::normalized(rms));
            });
    connect(controller_, &SessionController::status, this,
            [this](const QString &text) { statusLabel_->setText(text); });
    connect(controller_, &SessionController::error, this,
            [this](const QString &text) { statusLabel_->setText(text); });
    connect(controller_, &SessionController::captureFailed, this, [this](const QString &text) {
        setRunning(false);
        statusLabel_->setText(tr("Capture failed: %1").arg(text));
    });
}

MeetingWindow::~MeetingWindow() { stopRecording(); }

void MeetingWindow::buildUi() {
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(16, 12, 16, 14);
    body->setSpacing(8);

    // Header: the two animated state badges, then refresh and the settings
    // popover button. No device rows on screen — a meeting needs the
    // transcript, the knobs live one click away.
    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    micBadge_ = new LevelBadge(LevelBadge::Kind::Microphone);
    micBadge_->setObjectName(QStringLiteral("micLevelBadge"));
    micBadge_->setToolTip(tr("Microphone level"));
    remoteBadge_ = new LevelBadge(LevelBadge::Kind::Remote);
    remoteBadge_->setObjectName(QStringLiteral("remoteLevelBadge"));
    remoteBadge_->setToolTip(tr("Call audio level"));
    head->addWidget(micBadge_);
    head->addWidget(remoteBadge_);
    head->addStretch();
    auto *refresh = new QPushButton;
    refresh->setProperty("variant", "ghost");
    refresh->setProperty("size", "icon");
    refresh->setIcon(UiKit::icon("rotate-cw"));
    refresh->setFixedSize(30, 30);
    refresh->setIconSize(QSize(16, 16));
    refresh->setToolTip(tr("Refresh devices"));
    connect(refresh, &QPushButton::clicked, this, [this] { refreshDevices(); });
    head->addWidget(refresh);
    settingsButton_ = new QPushButton;
    settingsButton_->setProperty("variant", "ghost");
    settingsButton_->setProperty("size", "icon");
    settingsButton_->setIcon(UiKit::icon("cog"));
    settingsButton_->setFixedSize(30, 30);
    settingsButton_->setIconSize(QSize(16, 16));
    settingsButton_->setToolTip(tr("Meeting settings"));
    head->addWidget(settingsButton_);
    body->addLayout(head);

    // Tabs: free-form notes first, live transcript second — the same order
    // the saved document keeps.
    auto *tabs = new QHBoxLayout;
    tabs->setSpacing(6);
    tabNotes_ = new QPushButton(tr("Notes"));
    tabTranscript_ = new QPushButton(tr("Transcript"));
    for (QPushButton *tab : {tabNotes_, tabTranscript_}) {
        tab->setCheckable(true);
        tab->setProperty("variant", "ghost");
        tabs->addWidget(tab);
    }
    tabs->addStretch();
    body->addLayout(tabs);

    notes_ = new QTextEdit;
    notes_->setObjectName(QStringLiteral("meetingNotes"));
    notes_->setAcceptRichText(false);
    notes_->setPlaceholderText(tr("Type your meeting notes here — they are saved into the same document, above the transcript."));
    transcript_ = new QTextEdit;
    transcript_->setObjectName(QStringLiteral("meetingTranscript"));
    transcript_->setReadOnly(true);
    transcript_->setAcceptRichText(false);
    transcript_->setPlaceholderText(tr("Lines appear here with a timestamp and a speaker."));

    body->addWidget(notes_, 1);
    body->addWidget(transcript_, 1);

    connect(tabNotes_, &QPushButton::clicked, this, [this] {
        tabNotes_->setChecked(true);
        tabTranscript_->setChecked(false);
        notes_->setVisible(true);
        transcript_->setVisible(false);
    });
    connect(tabTranscript_, &QPushButton::clicked, this, [this] {
        tabTranscript_->setChecked(true);
        tabNotes_->setChecked(false);
        transcript_->setVisible(true);
        notes_->setVisible(false);
    });
    tabTranscript_->setChecked(true);
    notes_->setVisible(false);

    // Footer: start/stop and the manual save; the status explains auto-save.
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    startButton_ = new QPushButton(tr("Start"));
    startButton_->setObjectName(QStringLiteral("primary"));
    startButton_->setIcon(UiKit::icon("play"));
    saveButton_ = new QPushButton(tr("Save transcript…"));
    saveButton_->setProperty("variant", "ghost");
    saveButton_->setIcon(UiKit::icon("folder"));
    saveButton_->setEnabled(false);
    statusLabel_ = new QLabel(tr("Ready — press Start"));
    statusLabel_->setProperty("role", "muted");
    statusLabel_->setWordWrap(true);
    actions->addWidget(startButton_);
    actions->addWidget(saveButton_);
    actions->addWidget(statusLabel_, 1);
    body->addLayout(actions);

    connect(startButton_, &QPushButton::clicked, this, [this] {
        if (running_) {
            stopRecording();
        } else {
            startRecording();
        }
    });
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        const QString text = documentText();
        if (text.isEmpty()) return;
        const QString suggested = defaultFolder() + QStringLiteral("/Meeting %1.md").arg(fileNameStamp(sessionStartedAt_));
        const QString path = QFileDialog::getSaveFileName(this, tr("Save transcript"),
            suggested, tr("Markdown (*.md)"));
        if (path.isEmpty()) return;
        QFile out(path);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            out.write(text.toUtf8());
            statusLabel_->setText(tr("Saved to %1").arg(path));
        }
    });
    UiKit::wrapDialog(*this, windowTitle());
}

void MeetingWindow::buildSettings() {
    settings_ = new SettingsPopover(settingsButton_, tr("Meeting settings"));
    auto *form = settings_->form();

    micBox_ = new QComboBox;
    callBox_ = new QComboBox;
    form->addRow(tr("My microphone"), micBox_);
    form->addRow(tr("Call audio"), callBox_);

    languageBox_ = new QComboBox;
    languageBox_->addItem(tr("Mixed / auto"), int(0));
    languageBox_->addItem(tr("Russian"), int(1));
    languageBox_->addItem(tr("English"), int(2));
    form->addRow(tr("Meeting language"), languageBox_);

    diarize_ = new QCheckBox(tr("Separate remote speakers"));
    diarize_->setToolTip(tr("Labels remote speech with a separate diariser, so it works in any\n"
                           "language; the words still come from the recogniser for the meeting\n"
                           "language. Names can be changed from the transcript's context menu."));
    form->addRow(tr("Speakers"), diarize_);

    autoSave_ = new QCheckBox(tr("Auto-save on stop"));
    autoSave_->setChecked(app_->settings()->meetingAutoSave);
    form->addRow(tr("Saving"), autoSave_);

    auto *folderRow = new QHBoxLayout;
    folderRow->setSpacing(8);
    folderLabel_ = new QLabel;
    folderLabel_->setProperty("role", "muted");
    folderLabel_->setWordWrap(true);
    auto *choose = new QPushButton(tr("Choose…"));
    folderRow->addWidget(choose, 0);
    folderRow->addWidget(folderLabel_, 1);
    settings_->column()->addLayout(folderRow);

    const auto updateFolderLabel = [this] {
        folderLabel_->setText(defaultFolder()
            + QStringLiteral("\n«Meeting %1.md»").arg(
                fileNameStamp(sessionStartedAt_.isValid() ? sessionStartedAt_ : QDateTime::currentDateTime())));
    };
    updateFolderLabel();

    connect(choose, &QPushButton::clicked, this, [this, updateFolderLabel] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Transcripts folder"),
                                                              defaultFolder());
        if (dir.isEmpty()) return;
        app_->settings()->meetingTranscriptDir = dir;
        app_->settings()->save();
        updateFolderLabel();
    });
    connect(autoSave_, &QCheckBox::toggled, this, [this](bool on) {
        app_->settings()->meetingAutoSave = on;
        app_->settings()->save();
    });
    connect(settingsButton_, &QPushButton::clicked, this, [this] { settings_->openAt(); });
}

QString MeetingWindow::defaultFolder() const {
    const QString configured = app_->settings()->meetingTranscriptDir;
    return configured.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
              + QStringLiteral("/VoicePanel/Meetings")
        : configured;
}

void MeetingWindow::refreshDevices() {
    const Settings *s = app_->settings();
    micBox_->clear();
    for (const auto &device : WasapiCapture::listDevices(true)) {
        micBox_->addItem(device.second.isEmpty() ? tr("System default") : device.second, device.first);
    }
    callBox_->clear();
    for (const auto &device : WasapiCapture::listDevices(false)) {
        callBox_->addItem(device.second.isEmpty() ? tr("Default output") : device.second,
                          device.first.isEmpty() ? QStringLiteral("dev:") : QStringLiteral("dev:") + device.first);
    }
    if (int i = micBox_->findData(s->inputDevice); i >= 0) micBox_->setCurrentIndex(i);
    const QString wanted = s->systemAudioDevice.isEmpty() ? QStringLiteral("dev:")
                                                          : QStringLiteral("dev:") + s->systemAudioDevice;
    if (int i = callBox_->findData(wanted); i >= 0) callBox_->setCurrentIndex(i);
    languageBox_->setCurrentIndex(languageBox_->findData(s->meetingLanguage));
    diarize_->setChecked(s->meetingDiarize);
    autoSave_->setChecked(s->meetingAutoSave);
}

void MeetingWindow::applySettings() {
    Settings *s = app_->settings();
    s->inputDevice = micBox_->currentData().toString();
    const QString call = callBox_->currentData().toString();
    s->systemAudioDevice = call.startsWith(QLatin1String("dev:")) ? call.mid(4) : call;
    s->meetingLanguage = languageBox_->currentData().toInt();
    s->meetingDiarize = diarize_->isChecked();
    s->save();
}

void MeetingWindow::startRecording() {
    applySettings();
    transcript_->clear();
    lines_.clear();
    sessionStartedAt_ = QDateTime::currentDateTime();
    controller_->configure(*app_->settings(), SessionController::Mode::Meeting);
    QString vadPath = app_->vadPath();
    if (vadPath.isEmpty()) vadPath = app_->settings()->vadModel;
    if (vadPath.isEmpty()) vadPath = QStringLiteral("models/silero_vad.onnx");
    if (!controller_->start(vadPath)) {
        statusLabel_->setText(tr("Could not start capture — check the devices in Settings."));
        return;
    }
    setRunning(true);
}

void MeetingWindow::stopRecording() {
    if (!running_) return;
    controller_->stop();
    setRunning(false);
    // Auto-save: one Markdown document per meeting, Notes section first,
    // Transcript second, named after the full start time.
    if (!app_->settings()->meetingAutoSave || lines_.isEmpty()) return;
    const QString text = documentText();
    if (text.isEmpty()) return;
    QDir().mkpath(defaultFolder());
    const QString path = defaultFolder()
        + QStringLiteral("/Meeting %1.md").arg(fileNameStamp(sessionStartedAt_));
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        out.write(text.toUtf8());
        statusLabel_->setText(tr("Saved to %1").arg(path));
    } else {
        statusLabel_->setText(tr("Auto-save failed for %1").arg(path));
    }
}

void MeetingWindow::appendLine(const QString &speaker, const QString &text, double at) {
    lines_.append({speaker, text, at});
    const QString display = names_.value(speaker, speaker);
    transcript_->append(QStringLiteral("[%1] %2: %3").arg(clock(at), display, text));
}

QString MeetingWindow::transcriptText() const {
    QString out;
    for (const Line &line : lines_) {
        out += QStringLiteral("[%1] %2: %3\n")
                   .arg(clock(line.at), names_.value(line.speaker, line.speaker), line.text);
    }
    return out;
}

QString MeetingWindow::documentText() const {
    const QString stamp = sessionStartedAt_.isValid()
        ? sessionStartedAt_.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
        : QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    QString doc = QStringLiteral("# Meeting — %1\n").arg(stamp);
    const QString notes = notes_->toPlainText().trimmed();
    if (!notes.isEmpty()) {
        doc += QStringLiteral("\n## Notes\n%1\n").arg(notes);
    }
    const QString transcript = transcriptText();
    if (!transcript.isEmpty()) {
        doc += QStringLiteral("\n## Transcript\n%1").arg(transcript);
    }
    return doc;
}

void MeetingWindow::rebuildTranscript() {
    transcript_->clear();
    for (const Line &line : lines_) {
        transcript_->append(QStringLiteral("[%1] %2: %3")
                                .arg(clock(line.at), names_.value(line.speaker, line.speaker),
                                     line.text));
    }
}

void MeetingWindow::renameSpeaker(const QString &speaker) {
    bool ok = false;
    const QString current = names_.value(speaker, speaker);
    const QString name = QInputDialog::getText(this, tr("Rename speaker"), tr("Name for “%1”").arg(current),
                                               QLineEdit::Normal, current, &ok);
    if (!ok) return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == current) return;
    names_[speaker] = trimmed;
    rebuildTranscript();
}

void MeetingWindow::setRunning(bool running) {
    running_ = running;
    emit recordingChanged(running);
    startButton_->setText(running ? tr("Stop") : tr("Start"));
    startButton_->setIcon(UiKit::icon(running ? "stop" : "play"));
    if (!running) {
        micBadge_->reset();
        remoteBadge_->reset();
    }
    micBox_->setEnabled(!running);
    callBox_->setEnabled(!running);
    languageBox_->setEnabled(!running);
    diarize_->setEnabled(!running);
    saveButton_->setEnabled(!running && !controller_->transcript().isEmpty());
}

} // namespace VoicePanel
