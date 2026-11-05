#include "app/SessionWindow.h"
#include "app/PanelApp.h"
#include "app/SettingsPopover.h"
#include "app/UiKit.h"
#include "audio/WasapiCapture.h"
#include "core/AudioMeter.h"
#include "core/LevelBadge.h"
#include "core/SessionController.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizeGrip>
#include <QSplitter>
#include <QTextEdit>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

namespace VoicePanel {

namespace {
QString clock(double seconds) {
    const int total = int(seconds);
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

SessionWindow::SessionWindow(PanelApp *app, QWidget *parent)
    : QDialog(parent), app_(app) {
    setObjectName(QStringLiteral("voicePlayback"));
    setWindowIcon(UiKit::icon("mic"));
    setWindowTitle(tr("VoicePanel — live"));
    resize(704, 504);

    controller_ = new SessionController(app_->service(), app_->tts(), this);
    buildUi();
    buildSettings();
    refreshDevices();
    refreshVoices();
    updateExplanation();
    updateCableHint();

    connect(controller_, &SessionController::caption, this,
            [this](const QString &heard, const QString &translated) {
        if (mode_ == Mode::Interpreter) appendRemote(heard, translated);
        else appendRemote(heard, translated);   // subtitles: one pane, both lines
    });
    connect(controller_, &SessionController::outgoing, this,
            [this](const QString &said, const QString &voiced) {
        appendMine(said, voiced);
    });
    connect(controller_, &SessionController::status, this,
            [this](const QString &text) { statusLabel_->setText(text); });
    connect(controller_, &SessionController::error, this,
            [this](const QString &text) { statusLabel_->setText(text); });
    connect(controller_, &SessionController::captureFailed, this, [this](const QString &text) {
        // Unlock the device pickers: the session is over, not running blind.
        stopSession();
        statusLabel_->setText(tr("Capture failed: %1").arg(text));
    });
    connect(controller_, &SessionController::level, this, [this](double rms) {
        micBadge_->setRecording(true);
        micBadge_->setLevel(AudioMeter::normalized(rms));
    });
    connect(controller_, &SessionController::levelRemote, this, [this](double rms) {
        remoteBadge_->setRecording(true);
        remoteBadge_->setLevel(AudioMeter::normalized(rms));
    });
    connect(app_->tts(), &TtsPlayer::started, this,
            [this]() { statusLabel_->setText(tr("Preview: playing")); });
    connect(app_->tts(), &TtsPlayer::finished, this, [this]() {
        if (!running_) statusLabel_->setText(tr("Ready — press Start"));
    });
    connect(app_->tts(), &TtsPlayer::error, this,
            [this](const QString &message) { statusLabel_->setText(tr("Speech: %1").arg(message)); });
}

SessionWindow::~SessionWindow() { stopSession(); }

void SessionWindow::buildUi() {
    // Content only: UiKit::wrapDialog supplies the same shell, title row and
    // spacing every other dialog in the app uses.
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(16, 12, 16, 14);
    body->setSpacing(10);

    auto *modeRow = new QHBoxLayout;
    modeRow->setSpacing(8);
    tabInterpreter_ = new QPushButton(tr("Live interpreter"));
    tabInterpreter_->setIcon(UiKit::icon("languages"));
    tabMeeting_ = new QPushButton(tr("Subtitles"));
    tabMeeting_->setIcon(UiKit::icon("captions"));
    for (QPushButton *tab : {tabInterpreter_, tabMeeting_}) {
        tab->setCheckable(true);
        tab->setProperty("variant", "ghost");
        modeRow->addWidget(tab);
    }
    modeRow->addStretch();
    refreshButton_ = new QPushButton(tr("Refresh"));
    refreshButton_->setProperty("variant", "ghost");
    refreshButton_->setIcon(UiKit::icon("rotate-cw"));
    connect(refreshButton_, &QPushButton::clicked, this, [this] {
        refreshDevices();
        updateCableHint();
    });
    modeRow->addWidget(refreshButton_);
    settingsButton_ = new QPushButton(tr("Settings"));
    settingsButton_->setProperty("variant", "ghost");
    settingsButton_->setIcon(UiKit::icon("cog"));
    modeRow->addWidget(settingsButton_);
    body->addLayout(modeRow);

    connect(tabInterpreter_, &QPushButton::clicked, this, [this] {
        if (mode_ != Mode::Interpreter) {
            if (running_) stopSession();
            mode_ = Mode::Interpreter;
            updateExplanation();
            startListening();
        }
    });
    connect(tabMeeting_, &QPushButton::clicked, this, [this] {
        if (mode_ != Mode::Subtitles) {
            if (running_) stopSession();
            mode_ = Mode::Subtitles;
            updateExplanation();
            startListening();
        }
    });

    // ---- interpreter body: two equal halves with a swap on the divider ----
    interpreterBody_ = new QWidget;
    auto *sides = new QHBoxLayout(interpreterBody_);
    sides->setContentsMargins(0, 0, 0, 0);
    sides->setSpacing(10);

    const auto pane = [](LevelBadge::Kind kind, const QString &badgeName,
                         const QString &editName) -> QPair<QWidget *, QTextEdit *> {
        auto *box = new QWidget;
        auto *lay = new QVBoxLayout(box);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        auto *badge = new LevelBadge(kind);
        badge->setObjectName(badgeName);
        auto *head = new QHBoxLayout;
        head->setSpacing(6);
        head->addWidget(badge);
        head->addStretch();
        lay->addLayout(head);
        auto *edit = new QTextEdit;
        edit->setObjectName(editName);
        edit->setReadOnly(true);
        edit->setAcceptRichText(true);
        edit->setMinimumHeight(160);
        edit->setLineWrapMode(QTextEdit::WidgetWidth);
        lay->addWidget(edit, 1);
        return qMakePair(box, edit);
    };
    const auto remote = pane(LevelBadge::Kind::Remote,
                             QStringLiteral("remoteLevelBadge"), QStringLiteral("liveRemote"));
    remoteBadge_ = remote.first->findChild<LevelBadge *>();
    remoteEdit_ = remote.second;
    const auto mine = pane(LevelBadge::Kind::Microphone,
                           QStringLiteral("micLevelBadge"), QStringLiteral("liveMine"));
    micBadge_ = mine.first->findChild<LevelBadge *>();
    mineEdit_ = mine.second;

    auto *dividerBox = new QVBoxLayout;
    dividerBox->setSpacing(0);
    dividerBox->addStretch();
    sideDivider_ = new QFrame;
    sideDivider_->setFrameShape(QFrame::VLine);
    sideDivider_->setFrameShadow(QFrame::Plain);
    sideDivider_->setFixedWidth(1);
    dividerBox->addWidget(sideDivider_);
    swapButton_ = new QPushButton;
    swapButton_->setProperty("variant", "ghost");
    swapButton_->setProperty("size", "icon");
    swapButton_->setIcon(UiKit::icon("arrow-right-left"));
    swapButton_->setFixedSize(32, 32);
    swapButton_->setIconSize(QSize(16, 16));
    swapButton_->setToolTip(tr("Swap the languages"));
    dividerBox->addWidget(swapButton_);
    dividerBox->addStretch();
    mineBox_ = mine.first;
    sides->addWidget(remote.first, 1);
    sides->addLayout(dividerBox);
    sides->addWidget(mineBox_, 1);
    // Subtitles mode reuses this body: the microphone half hides and the
    // call half stretches across, so one widget tree serves both modes.
    body->addWidget(interpreterBody_, 1);

    connect(swapButton_, &QPushButton::clicked, this, [this] {
        const int flipped = callLanguageBox_->currentIndex() == 0 ? 1 : 0;
        callLanguageBox_->setCurrentIndex(flipped);
        if (running_) {
            stopSession();
            startListening();
        }
    });

    // ---- footer ----------------------------------------------------------
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    startButton_ = new QPushButton(tr("Start"));
    startButton_->setObjectName(QStringLiteral("primary"));
    startButton_->setIcon(UiKit::icon("play"));
    statusLabel_ = new QLabel(tr("Ready — press Start"));
    statusLabel_->setProperty("role", "muted");
    statusLabel_->setWordWrap(true);
    actions->addWidget(startButton_);
    actions->addWidget(statusLabel_, 1);
    body->addLayout(actions);

    connect(startButton_, &QPushButton::clicked, this, [this] {
        if (running_) {
            stopSession();
        } else {
            startListening();
        }
    });
    // updateExplanation/updateCableHint run from the constructor AFTER
    // buildSettings(): they touch the popover combos.
    UiKit::wrapDialog(*this, windowTitle());
}

void SessionWindow::buildSettings() {
    settings_ = new SettingsPopover(settingsButton_, tr("Interpreter settings"));
    auto *form = settings_->form();

    micBox_ = new QComboBox;
    callBox_ = new QComboBox;
    form->addRow(tr("My microphone"), micBox_);
    form->addRow(tr("Call audio"), callBox_);

    callLanguageBox_ = new QComboBox;
    callLanguageBox_->addItem(tr("English — my Russian becomes English"), int(0));
    callLanguageBox_->addItem(tr("Russian — my English becomes Russian"), int(1));
    form->addRow(tr("The other person speaks"), callLanguageBox_);

    outgoingBox_ = new QComboBox;
    outgoingBox_->addItem(tr("Translated"), int(0));
    outgoingBox_->addItem(tr("As I said it"), int(1));
    outgoingBox_->addItem(tr("Captions only"), int(2));
    form->addRow(tr("My voice goes"), outgoingBox_);

    speakBox_ = new QComboBox;
    form->addRow(tr("Play into"), speakBox_);

    voiceBox_ = new QComboBox;
    form->addRow(tr("Voice profile"), voiceBox_);

    auto *voiceRow = new QHBoxLayout;
    voiceRow->setSpacing(8);
    previewButton_ = new QPushButton(tr("Preview voice"));
    previewButton_->setIcon(UiKit::icon("audio-lines"));
    previewButton_->setProperty("variant", "ghost");
    voiceRow->addWidget(previewButton_);
    form->addRow(QString(), voiceRow);

    cableHint_ = new QLabel;
    cableHint_->setWordWrap(true);
    cableHint_->setProperty("role", "muted");
    settings_->column()->addWidget(cableHint_);

    connect(settingsButton_, &QPushButton::clicked, this, [this] { settings_->openAt(); });
    connect(callLanguageBox_, &QComboBox::currentIndexChanged, this, [this] {
        if (!isVisible()) return;
        applySettings();
    });
    connect(previewButton_, &QPushButton::clicked, this, [this] {
        const QString profile = voiceBox_->currentData().toString();
        if (profile.isEmpty()) {
            statusLabel_->setText(tr("Record a voice profile first (Settings → Voice profiles)."));
            return;
        }
        app_->tts()->setOutputDevice(speakBox_->currentData().toInt());
        app_->tts()->setModel(app_->settings()->ttsModel);
        app_->tts()->setLanguageVoices(profile, profile);
        app_->tts()->setSpeed(app_->settings()->ttsSpeed);
        app_->tts()->stop();
        statusLabel_->setText(tr("Preview: speaking…"));
        app_->tts()->speak(tr("This is how I will sound on the call."));
    });
}

void SessionWindow::updateExplanation() {
    const bool interpreter = mode_ == Mode::Interpreter;
    tabInterpreter_->setChecked(interpreter);
    tabMeeting_->setChecked(!interpreter);   // Subtitles
    setWindowTitle(interpreter ? tr("VoicePanel — live interpreter")
                               : tr("VoicePanel — subtitles"));
    // In subtitles mode there is no "my side": the microphone half hides and
    // the call half takes the full width, and the speaking-only settings
    // disappear from the popover.
    mineBox_->setVisible(interpreter);
    sideDivider_->setVisible(interpreter);
    swapButton_->setVisible(interpreter);
    if (settings_ != nullptr) {
        settings_->form()->labelForField(micBox_)->setVisible(interpreter);
        micBox_->setVisible(interpreter);
        settings_->form()->setRowVisible(outgoingBox_, interpreter);
        settings_->form()->setRowVisible(voiceBox_, interpreter);
        settings_->form()->setRowVisible(speakBox_, interpreter);
        settings_->form()->setRowVisible(previewButton_, interpreter);
        settings_->form()->setRowVisible(callLanguageBox_, interpreter);
    }
    if (!interpreter) {
        clearPanes();
    }
}

void SessionWindow::refreshDevices() {
    const Settings *s = app_->settings();
    micBox_->clear();
    for (const auto &device : WasapiCapture::listDevices(true)) {
        micBox_->addItem(device.second.isEmpty() ? tr("System default") : device.second, device.first);
    }
    callBox_->clear();
    // Applications first: capturing one app is what isolates a call from every
    // other sound, and it needs no virtual device.
    // Section rows are disabled: they are labels, not choices. Leaving one
    // selectable is how "section" once ended up saved as the device name.
    const auto addSection = [this](const QString &title) {
        callBox_->addItem(title, QString());
        const int row = callBox_->count() - 1;
        callBox_->setItemData(row, false, Qt::UserRole - 1);
    };
    const auto apps = WasapiCapture::processLoopbackSupported()
                          ? WasapiCapture::listAudioProcesses()
                          : QList<QPair<quint32, QString>>();
    if (!apps.isEmpty()) {
        addSection(tr("Applications"));
        for (const auto &app : apps) {
            callBox_->addItem(app.second, QStringLiteral("pid:%1").arg(app.first));
        }
        addSection(tr("Output devices (whole device)"));
    }
    for (const auto &device : WasapiCapture::listDevices(false)) {
        callBox_->addItem(device.second.isEmpty() ? tr("Default output") : device.second,
                          device.first.isEmpty() ? QStringLiteral("dev:default")
                                                 : QStringLiteral("dev:") + device.first);
    }
    speakBox_->clear();
    for (const auto &device : TtsPlayer::listOutputDevices()) {
        speakBox_->addItem(device.second, device.first);
    }
    if (int i = micBox_->findData(s->inputDevice); i >= 0) micBox_->setCurrentIndex(i);
    const QString saved = s->systemAudioDevice.startsWith(QLatin1String("dev:"))
                              ? s->systemAudioDevice
                              : (s->systemAudioDevice.isEmpty() ? QStringLiteral("dev:default")
                                                               : QStringLiteral("pid:%1").arg(s->systemAudioDevice));
    int row = callBox_->findData(saved);
    if (row < 0) row = callBox_->findData(QStringLiteral("dev:default"));
    if (row < 0 && callBox_->count() > 0) row = callBox_->count() - 1;
    if (row >= 0) callBox_->setCurrentIndex(row);
    if (int i = speakBox_->findData(s->outputDevice); i >= 0) speakBox_->setCurrentIndex(i);
}

void SessionWindow::refreshVoices() {
    const Settings *s = app_->settings();
    voiceBox_->clear();
    const QString current = s->ttsEnglishVoice.isEmpty() ? s->ttsVoiceProfile : s->ttsEnglishVoice;
    if (!current.isEmpty()) voiceBox_->addItem(current, current);
    else voiceBox_->addItem(tr("No voice profile yet"), QString());

    QPointer<SessionWindow> self(this);
    app_->service()->getJson(QStringLiteral("/v1/voices"),
                             [this, self, current](bool ok, QByteArray body, QString) {
        if (!self || !ok) return;
        const auto arr = QJsonDocument::fromJson(body).object().value("voices").toArray();
        if (arr.isEmpty()) return;
        const QString keep = voiceBox_->currentData().toString();
        voiceBox_->clear();
        for (const auto &value : arr) {
            const auto profile = value.toObject();
            const QString id = profile.value("id").toString();
            if (!id.isEmpty()) voiceBox_->addItem(profile.value("name").toString(), id);
        }
        int index = voiceBox_->findData(keep.isEmpty() ? current : keep);
        if (index < 0) index = 0;
        if (voiceBox_->count() > 0) voiceBox_->setCurrentIndex(index);
    });
}

void SessionWindow::updateCableHint() {
    if (speakBox_ == nullptr) return;
    static const QRegularExpression virtualName(
        QStringLiteral("cable|virtual|broadcast|arena|voicemeeter|vac|loopback"),
        QRegularExpression::CaseInsensitiveOption);
    bool found = false;
    for (int i = 0; i < speakBox_->count(); ++i) {
        if (virtualName.match(speakBox_->itemText(i)).hasMatch()) {
            found = true;
            break;
        }
    }
    cableHint_->setVisible(!found);
    cableHint_->setText(found ? QString() : tr(
        "No virtual audio cable found. Audio-interface software often installs one (a playback "
        "endpoint paired with a matching recording endpoint); otherwise install VB-CABLE (free, "
        "restart Windows once). Play into the cable's output here, and choose its matching input "
        "as the microphone in the call application."));
}

void SessionWindow::applySettings() {
    Settings *s = app_->settings();
    s->inputDevice = micBox_->currentData().toString();
    const QString call = callBox_->currentData().toString();
    // "dev:default" is the default endpoint, which is the empty id.
    if (call.isEmpty()) {
        s->systemAudioDevice.clear();
    } else if (call.startsWith(QLatin1String("dev:"))) {
        const QString id = call.mid(4);
        s->systemAudioDevice = id == QLatin1String("default") ? QString() : id;
    } else if (call.startsWith(QLatin1String("pid:"))) {
        s->systemAudioDevice = call;
    }
    s->outputDevice = speakBox_->currentData().toInt();
    s->liveOutgoing = outgoingBox_->currentData().toInt();
    const int callLang = callLanguageBox_->currentData().toInt();
    s->callLanguage = callLang;
    const QString profile = voiceBox_->currentData().toString();
    if (!profile.isEmpty()) {
        s->ttsVoiceProfile = profile;
        s->ttsEnglishVoice = profile;
    }
    s->save();
}

void SessionWindow::startListening() {
    applySettings();
    previousOutputDevice_ = app_->tts()->outputDevice();
    app_->tts()->setOutputDevice(app_->settings()->outputDevice);

    clearPanes();
    controller_->configure(*app_->settings(),
                           mode_ == Mode::Interpreter ? SessionController::Mode::Interpreter
                                                      : SessionController::Mode::Subtitles);
    controller_->setOutgoing(SessionController::Outgoing(app_->settings()->liveOutgoing));
    QString vadPath = app_->vadPath();
    if (vadPath.isEmpty()) vadPath = app_->settings()->vadModel;
    if (vadPath.isEmpty()) vadPath = QStringLiteral("models/silero_vad.onnx");
    if (!controller_->start(vadPath)) {
        statusLabel_->setText(tr("Could not start capture — check the devices in Settings."));
        return;
    }
    setRunning(true);
}

void SessionWindow::appendRemote(const QString &heard, const QString &translated) {
    // Amber marker: the call side. The translation reads heavier than the
    // heard line, but the two sizes stay close (the approved design).
    remoteEdit_->append(QStringLiteral(
        "<span style=\"color:#ee9a3a\">&#9679;</span> "
        "<span style=\"color:#c9bd9a\">%1</span><br><b>%2</b>")
        .arg(heard.toHtmlEscaped(), translated.toHtmlEscaped()));
}

void SessionWindow::appendMine(const QString &said, const QString &voiced) {
    mineEdit_->append(QStringLiteral(
        "<span style=\"color:#3fa37a\">&#9679;</span> "
        "<span style=\"color:#c9bd9a\">%1</span><br><b>%2</b>")
        .arg(said.toHtmlEscaped(), voiced.isEmpty() ? tr("(not voiced)") : voiced.toHtmlEscaped()));
}

void SessionWindow::clearPanes() {
    remoteEdit_->clear();
    mineEdit_->clear();
}

void SessionWindow::setRunning(bool running) {
    running_ = running;
    emit recordingChanged(running);
    startButton_->setText(running ? tr("Stop") : tr("Start"));
    startButton_->setIcon(UiKit::icon(running ? "stop" : "play"));
    if (!running) {
        micBadge_->reset();
        remoteBadge_->reset();
    }
    settingsButton_->setEnabled(true);
    micBox_->setEnabled(!running);
    callBox_->setEnabled(!running);
    speakBox_->setEnabled(!running);
    outgoingBox_->setEnabled(!running);
    voiceBox_->setEnabled(!running);
    previewButton_->setEnabled(!running);
    callLanguageBox_->setEnabled(!running);
    tabInterpreter_->setEnabled(!running);
    tabMeeting_->setEnabled(!running);
}

void SessionWindow::stopSession() {
    if (!running_) return;
    controller_->stop();
    setRunning(false);
    if (previousOutputDevice_ >= 0) app_->tts()->setOutputDevice(previousOutputDevice_);
}

void SessionWindow::openMode(Mode mode) {
    mode_ = mode;
    refreshDevices();
    updateExplanation();
    updateCableHint();
    show();
    raise();
    activateWindow();
    // Opening a mode shows the UI, never starts capturing: the user pressed a
    // button to look, not to be recorded.
    statusLabel_->setText(tr("Ready — press Start"));
}

QString SessionWindow::settingsSafeName() const {
    return QStringLiteral("voicepanel-%1.md")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
}

} // namespace VoicePanel
