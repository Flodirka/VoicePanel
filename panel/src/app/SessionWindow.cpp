#include "app/SessionWindow.h"
#include "app/PanelApp.h"
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
    refreshDevices();
    refreshVoices();
    updateExplanation();
    updateCableHint();

    connect(controller_, &SessionController::caption, this,
            [this](const QString &heard, const QString &translated) {
        appendHeard(tr("◀ %1").arg(heard));
        appendTranslated(tr("◀ %1").arg(translated));
    });
    connect(controller_, &SessionController::outgoing, this,
            [this](const QString &said, const QString &voiced) {
        appendHeard(tr("▶ %1").arg(said));
        appendTranslated(voiced.isEmpty() ? tr("▶ (not voiced)") : tr("▶ %1").arg(voiced));
    });
    connect(controller_, &SessionController::line, this,
            [this](const QString &speaker, const QString &text, double at) {
        appendLine(QStringLiteral("[%1] %2: %3").arg(clock(at), speaker, text));
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
    body->setContentsMargins(24, 20, 24, 20);
    body->setSpacing(12);

    const auto line = [] {
        auto *frame = new QFrame;
        frame->setObjectName(QStringLiteral("dialogSeparator"));
        frame->setFrameShape(QFrame::HLine);
        frame->setFrameShadow(QFrame::Plain);
        frame->setFixedHeight(1);
        return frame;
    };

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
    // Two animated state badges next to the tabs: the microphone fills while
    // it records, the speaker glyph reacts to call audio. Unlike the old bar
    // meters they decay on their own timer, so a silent loopback never leaves
    // a frozen bar behind.
    micBadge_ = new LevelBadge(LevelBadge::Kind::Microphone);
    micBadge_->setObjectName(QStringLiteral("micLevelBadge"));
    micBadge_->setToolTip(tr("Microphone level"));
    remoteBadge_ = new LevelBadge(LevelBadge::Kind::Remote);
    remoteBadge_->setObjectName(QStringLiteral("remoteLevelBadge"));
    remoteBadge_->setToolTip(tr("Call audio level"));
    modeRow->addWidget(micBadge_);
    modeRow->addWidget(remoteBadge_);
    modeRow->addStretch();
    refreshButton_ = new QPushButton(tr("Refresh"));
    refreshButton_->setProperty("variant", "ghost");
    refreshButton_->setIcon(UiKit::icon("rotate-cw"));
    connect(refreshButton_, &QPushButton::clicked, this, [this] {
        refreshDevices();
        updateCableHint();
    });
    modeRow->addWidget(refreshButton_);
    settingsToggle_ = new QPushButton;
    settingsToggle_->setText(tr("Settings"));
    settingsToggle_->setCheckable(true);
    settingsToggle_->setProperty("variant", "ghost");
    settingsToggle_->setIcon(UiKit::icon("cog"));
    modeRow->addWidget(settingsToggle_);
    body->addLayout(modeRow);
    body->addWidget(line());

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

    // ---- settings (collapsed), label-left rows like the settings dialog ----
    settingsBox_ = new QWidget;
    auto *form = new QFormLayout(settingsBox_);
    settingsForm_ = form;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    micBox_ = new QComboBox;
    callBox_ = new QComboBox;
    speakBox_ = new QComboBox;
    form->addRow(tr("My microphone"), micBox_);
    form->addRow(tr("Call audio"), callBox_);
    form->addRow(tr("Voice goes to"), speakBox_);

    callLanguageBox_ = new QComboBox;
    callLanguageBox_->addItem(tr("English — my Russian becomes English"), int(0));
    callLanguageBox_->addItem(tr("Russian — my English becomes Russian"), int(1));
    form->addRow(tr("The other person speaks"), callLanguageBox_);

    outgoingBox_ = new QComboBox;
    outgoingBox_->addItem(tr("Translated into English"), int(0));
    outgoingBox_->addItem(tr("As I said it"), int(1));
    outgoingBox_->addItem(tr("Captions only"), int(2));
    voiceBox_ = new QComboBox;
    auto *voiceRow = new QHBoxLayout;
    voiceRow->setSpacing(8);
    voiceRow->addWidget(outgoingBox_, 1);
    voiceRow->addWidget(voiceBox_, 1);
    previewButton_ = new QPushButton(tr("Preview"));
    previewButton_->setIcon(UiKit::icon("audio-lines"));
    previewButton_->setProperty("variant", "ghost");
    voiceRow->addWidget(previewButton_);
    form->addRow(tr("Output voice"), voiceRow);

    cableHint_ = new QLabel;
    cableHint_->setWordWrap(true);
    cableHint_->setProperty("role", "muted");
    form->addRow(cableHint_);
    settingsBox_->setVisible(false);
    body->addWidget(settingsBox_);

    connect(settingsToggle_, &QPushButton::toggled, this, [this](bool open) {
        settingsBox_->setVisible(open);
        adjustSize();
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

    // ---- two columns -----------------------------------------------------
    columns_ = new QSplitter(Qt::Horizontal);
    columns_->setObjectName(QStringLiteral("comparison"));
    columns_->setChildrenCollapsible(false);
    const auto addPane = [](const QString &caption, const QString &name) {
        auto *column = new QWidget;
        auto *layout = new QVBoxLayout(column);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *label = new QLabel(caption);
        label->setProperty("role", "muted");
        layout->addWidget(label);
        auto *edit = new QTextEdit;
        edit->setObjectName(name);
        edit->setReadOnly(true);
        edit->setAcceptRichText(false);
        edit->setMinimumHeight(160);
        edit->setLineWrapMode(QTextEdit::WidgetWidth);
        layout->addWidget(edit, 1);
        return qMakePair(column, label);
    };
    const auto heard = addPane(tr("Said / heard"), QStringLiteral("liveHeard"));
    heardLabel_ = heard.second;
    heardEdit_ = heard.first->findChild<QTextEdit *>(QStringLiteral("liveHeard"));
    const auto translated = addPane(tr("Voiced / subtitles"), QStringLiteral("liveTranslated"));
    translatedLabel_ = translated.second;
    translatedEdit_ = translated.first->findChild<QTextEdit *>(QStringLiteral("liveTranslated"));
    translatedPane_ = translated.first;
    columns_->addWidget(heard.first);
    columns_->addWidget(translatedPane_);
    body->addWidget(columns_, 1);

    // ---- footer ----------------------------------------------------------
    body->addWidget(line());
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
    updateExplanation();
    updateCableHint();
    UiKit::wrapDialog(*this, windowTitle());
}

QString SessionWindow::settingsSafeName() const {
    return QStringLiteral("voicepanel-%1.md")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
}

void SessionWindow::updateExplanation() {
    const bool interpreter = mode_ == Mode::Interpreter;
    tabInterpreter_->setChecked(interpreter);
    tabMeeting_->setChecked(!interpreter);   // Subtitles
    // The shell draws the title from the window title; this label no longer
    // exists. Setting it here is what crashed both live modes.
    setWindowTitle(interpreter ? tr("VoicePanel — live interpreter")
                               : tr("VoicePanel — subtitles"));

    heardLabel_->setText(interpreter ? tr("Said / heard  (▶ me   ◀ call)") : tr("Heard"));
    // Subtitles only listen: hide every microphone and outgoing-voice row
    // instead of leaving dead controls on screen.
    // Hiding the widget alone leaves its row label behind — an orphaned
    // caption over empty space. Hide the whole row instead.
    settingsForm_->setRowVisible(micBox_, interpreter);
    settingsForm_->setRowVisible(speakBox_, interpreter);
    settingsForm_->setRowVisible(outgoingBox_, interpreter);
    settingsForm_->setRowVisible(voiceBox_, interpreter);
    translatedPane_->setVisible(interpreter);
    previewButton_->setVisible(interpreter);
    // Meeting controls belong to the meeting window now.
    settingsForm_->setRowVisible(callLanguageBox_, interpreter);
    callLanguageBox_->setCurrentIndex(callLanguageBox_->findData(app_->settings()->callLanguage));
    if (!interpreter) {
        heardEdit_->clear();
        translatedEdit_->clear();
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

    heardEdit_->clear();
    translatedEdit_->clear();
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

void SessionWindow::appendHeard(const QString &text) {
    heardEdit_->append(text);
}

void SessionWindow::appendTranslated(const QString &text) {
    translatedEdit_->append(text);
}

void SessionWindow::appendLine(const QString &text) {
    heardEdit_->append(text);
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
    settingsToggle_->setEnabled(true);
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
    // Opening a mode shows the settings, never starts capturing: the user
    // pressed a button to look, not to be recorded.
    statusLabel_->setText(tr("Ready — press Start"));
}

} // namespace VoicePanel
