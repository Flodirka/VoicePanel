#include "app/MeetingWindow.h"
#include "app/PanelApp.h"
#include "app/UiKit.h"
#include "audio/WasapiCapture.h"
#include "core/AudioMeter.h"
#include "core/LevelBadge.h"
#include "core/SessionController.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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
} // namespace

MeetingWindow::MeetingWindow(PanelApp *app, QWidget *parent) : QDialog(parent), app_(app) {
    setObjectName(QStringLiteral("voicePlayback"));   // shared dialog surface
    setWindowIcon(UiKit::icon("users"));
    setWindowTitle(tr("VoicePanel — meeting transcript"));
    resize(720, 560);
    controller_ = new SessionController(app_->service(), app_->tts(), this);
    buildUi();
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
    body->setContentsMargins(24, 20, 24, 20);
    body->setSpacing(12);

    // Settings are collapsed by default, as in the live window: the transcript
    // is what a meeting needs on screen, not six device rows.
    auto *topRow = new QHBoxLayout;
    // Two animated state badges next to the buttons: the microphone fills
    // while it records, the speaker glyph reacts to call audio. Unlike the
    // old bar meters they decay on their own timer, so a silent loopback
    // never leaves a frozen bar behind.
    micBadge_ = new LevelBadge(LevelBadge::Kind::Microphone);
    micBadge_->setObjectName(QStringLiteral("micLevelBadge"));
    micBadge_->setToolTip(tr("Microphone level"));
    remoteBadge_ = new LevelBadge(LevelBadge::Kind::Remote);
    remoteBadge_->setObjectName(QStringLiteral("remoteLevelBadge"));
    remoteBadge_->setToolTip(tr("Call audio level"));
    topRow->addWidget(micBadge_);
    topRow->addWidget(remoteBadge_);
    topRow->addStretch();
    auto *refresh = new QPushButton(tr("Refresh"));
    refresh->setProperty("variant", "ghost");
    refresh->setIcon(UiKit::icon("rotate-cw"));
    topRow->addWidget(refresh);
    auto *toggle = new QPushButton(tr("Settings"));
    toggle->setProperty("variant", "ghost");
    toggle->setIcon(UiKit::icon("cog"));
    toggle->setCheckable(true);
    toggle->setChecked(false);
    topRow->addWidget(toggle);
    body->addLayout(topRow);

    auto *settingsBox = new QWidget;
    auto *form = new QFormLayout(settingsBox);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    micBox_ = new QComboBox;
    callBox_ = new QComboBox;
    languageBox_ = new QComboBox;
    languageBox_->addItem(tr("Mixed / auto"), int(0));
    languageBox_->addItem(tr("Russian"), int(1));
    languageBox_->addItem(tr("English"), int(2));
    languageBox_->setCurrentIndex(languageBox_->findData(app_->settings()->meetingLanguage));
    diarize_ = new QCheckBox(tr("Separate remote speakers"));
    diarize_->setToolTip(tr("Labels remote speech with a separate diariser, so it works in any\n"
                           "language; the words still come from the recogniser for the meeting\n"
                           "language. Names can be changed from the transcript's context menu."));
    diarize_->setChecked(app_->settings()->meetingDiarize);
    form->addRow(tr("My microphone"), micBox_);
    form->addRow(tr("Call audio"), callBox_);
    form->addRow(tr("Meeting language"), languageBox_);
    form->addRow(tr("Speakers"), diarize_);
    settingsBox->setVisible(false);
    body->addWidget(settingsBox);
    connect(toggle, &QPushButton::toggled, this, [this, settingsBox](bool open) {
        settingsBox->setVisible(open);
        adjustSize();
    });
    connect(refresh, &QPushButton::clicked, this, [this] { refreshDevices(); });

    transcript_ = new QTextEdit;
    transcript_->setReadOnly(true);
    transcript_->setAcceptRichText(false);
    transcript_->setMinimumHeight(240);
    transcript_->setPlaceholderText(tr("Lines appear here with a timestamp and a speaker."));
    body->addWidget(transcript_, 1);

    auto *actions = new QHBoxLayout;
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
        const QString text = transcriptText();
        if (text.isEmpty()) return;
        const QString path = QFileDialog::getSaveFileName(this, tr("Save transcript"),
                                                          settingsSafeName(),
                                                          tr("Markdown (*.md);;Text (*.txt)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            statusLabel_->setText(tr("Cannot write %1").arg(path));
            return;
        }
        QTextStream out(&file);
        out << "# VoicePanel meeting\n\n" << text;
        statusLabel_->setText(tr("Saved %1").arg(path));
    });

    UiKit::wrapDialog(*this, windowTitle());
}

QString MeetingWindow::settingsSafeName() const {
    return QStringLiteral("meeting-%1.md")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
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
}

void MeetingWindow::applySettings() {
    Settings *s = app_->settings();
    s->inputDevice = micBox_->currentData().toString();
    const QString call = callBox_->currentData().toString();
    s->systemAudioDevice = call.startsWith(QLatin1String("dev:")) ? call.mid(4) : call;
    s->meetingLanguage = languageBox_->currentData().toInt();
    s->meetingDiarize = diarize_->isChecked();
    s->meetingModel = QStringLiteral("nemotron-diar");
    s->save();
}

void MeetingWindow::startRecording() {
    applySettings();
    transcript_->clear();
    lines_.clear();
    controller_->configure(*app_->settings(), SessionController::Mode::Meeting);
    QString vadPath = app_->vadPath();
    if (vadPath.isEmpty()) vadPath = app_->settings()->vadModel;
    if (vadPath.isEmpty()) vadPath = QStringLiteral("models/silero_vad.onnx");
    if (!controller_->start(vadPath)) {
        statusLabel_->setText(tr("Could not start capture — check the devices above."));
        return;
    }
    setRunning(true);
}

void MeetingWindow::stopRecording() {
    if (!running_) return;
    controller_->stop();
    setRunning(false);
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
