#include "app/SettingsDialog.h"

#include "app/PanelApp.h"
#include "app/UiKit.h"
#include "core/HotkeyManager.h"
#include "core/ServiceClient.h"
#include "audio/WasapiCapture.h"
#include "core/DictationController.h"
#include "core/MarkdownStore.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"
#include "core/VoiceProfiles.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <QPointer>
#include <QScreen>
#include <QSizeGrip>
#include <QSignalBlocker>
#include <windows.h>
#include <mmsystem.h>

using namespace VoicePanel;

namespace {

// Encode float32 mono samples as a 16-bit PCM WAV image.
QByteArray wavFromSamples(const QVector<float> &samples, int sampleRate) {
    QByteArray out;
    out.reserve(44 + samples.size() * 2);
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = quint32(samples.size() * 2);
    ds.writeRawData("RIFF", 4);
    ds << quint32(36 + dataBytes);
    ds.writeRawData("WAVE", 4);
    ds.writeRawData("fmt ", 4);
    ds << quint32(16) << quint16(1) << quint16(1) << quint32(sampleRate)
       << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    ds.writeRawData("data", 4);
    ds << dataBytes;
    for (float s : samples) {
        const qint16 v = qint16(qBound(-1.0f, s, 1.0f) * 32767.0f);
        ds << v;
    }
    return out;
}

// Modal recorder for a short clone reference: microphone -> 16 kHz mono WAV.
class RecordVoiceDialog : public QDialog {
public:
    explicit RecordVoiceDialog(const QString &device, QWidget *parent) : QDialog(parent) {
        setWindowTitle(tr("Record a voice reference"));
        setObjectName("voiceReferenceRecorder");
        setMinimumWidth(440);
        auto *lay = new QVBoxLayout(this);
        auto *hint = new QLabel(tr("Speak 5–20 seconds in a steady, natural voice. "
                                   "A quiet room and a normal speaking pace work best."), this);
        hint->setWordWrap(true);
        lay->addWidget(hint);
        language_ = new QComboBox;
        language_->setObjectName("referenceLanguage");
        language_->addItem(tr("Russian reading"), "ru");
        language_->addItem(tr("English reading"), "en");
        lay->addWidget(language_);
        script_ = new QPlainTextEdit;
        script_->setObjectName("referenceTranscript");
        script_->setMinimumHeight(150);
        script_->setMaximumHeight(220);
        lay->addWidget(script_);
        const auto selectScript = [this]() {
            script_->setPlainText(language_->currentData() == "ru"
                ? QString::fromUtf8("Сегодня я спокойно рассказываю о своей работе. Я проектирую игры, проверяю правила и объясняю сложные идеи простыми словами. Иногда нужно уточнить детали, задать вопрос или исправить ошибку. Пусть голос звучит естественно, чётко и без спешки.")
                : QString("Today I am speaking in my usual voice, at a comfortable pace. I design games, write code, and explain ideas clearly. Some sentences are short; others need a little more detail. Does everything sound natural? Let's check one more example."));
            wavBytes_.clear();
            samples_.clear();
            if (stopBtn_) stopBtn_->setEnabled(false);
        };
        connect(language_, &QComboBox::currentIndexChanged, this, selectScript);
        selectScript();
        auto *scriptHint = new QLabel(tr("Read the text exactly, then pause briefly before stopping. You can edit it to match what you actually said."));
        scriptHint->setWordWrap(true);
        scriptHint->setProperty("role", "caption");
        lay->addWidget(scriptHint);
        status_ = new QLabel(tr("Ready"), this);
        status_->setStyleSheet("font-weight: 600;");
        lay->addWidget(status_);
        auto *row = new QHBoxLayout;
        startBtn_ = new QPushButton(tr("Start recording"), this);
        startBtn_->setProperty("variant", "primary");
        stopBtn_ = new QPushButton(tr("Stop and use"), this);
        stopBtn_->setEnabled(false);
        row->addWidget(startBtn_);
        row->addWidget(stopBtn_);
        lay->addLayout(row);
        auto *cancelBtn = new QPushButton(tr("Cancel"), this);
        lay->addWidget(cancelBtn);

        capture_ = new WasapiCapture(this);
        capture_->setDevice(device);
        capture_->setSampleRate(kReferenceRate);
        connect(capture_, &WasapiCapture::framesAvailable, this,
                [this](const QVector<float> &samples, double) {
                    if (!recording_) return;
                    samples_ += samples;
                    const double seconds = double(samples_.size()) / kReferenceRate;
                    stopBtn_->setEnabled(seconds >= 2.0);
                    status_->setText(tr("Recording… %1 s").arg(seconds, 0, 'f', 1));
                    if (seconds >= 60.0) stopRecording();
                });
        connect(capture_, &WasapiCapture::error, this, [this](const QString &e) {
            status_->setText(tr("Microphone error: %1").arg(e));
            stopRecording();
        });
        connect(startBtn_, &QPushButton::clicked, this, [this]() { startRecording(); });
        connect(stopBtn_, &QPushButton::clicked, this, [this]() {
            stopRecording();
            if (wavBytes_.isEmpty()) return;
            accept();
        });
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        UiKit::wrapDialog(*this, windowTitle());
    }

    QByteArray wavBytes() const { return wavBytes_; }
    QString referenceText() const { return script_->toPlainText().trimmed(); }
    QString language() const { return language_->currentData().toString(); }

protected:
    void reject() override {   // Esc or window close while recording
        if (recording_) stopRecording();
        QDialog::reject();
    }

private:
    void startRecording() {
        samples_.clear();
        wavBytes_.clear();
        if (!capture_->start(WasapiCapture::Source::Microphone)) {
            status_->setText(tr("Cannot open the microphone"));
            return;
        }
        recording_ = true;
        language_->setEnabled(false);
        script_->setReadOnly(true);
        startBtn_->setEnabled(false);
        stopBtn_->setEnabled(false);
        status_->setText(tr("Recording… 0.0 s"));
    }

    void stopRecording() {
        if (!recording_ || draining_) return;
        draining_ = true;
        capture_->stop();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        recording_ = false;
        draining_ = false;
        language_->setEnabled(true);
        script_->setReadOnly(false);
        samples_.resize(qMin(samples_.size(), qsizetype(kReferenceRate) * 60));
        const double seconds = double(samples_.size()) / kReferenceRate;
        if (seconds < 2.0) {
            status_->setText(tr("Too short (%1 s) — record at least 2 seconds")
                                 .arg(seconds, 0, 'f', 1));
            startBtn_->setEnabled(true);
            stopBtn_->setEnabled(false);
            return;
        }
        wavBytes_ = wavFromSamples(samples_, kReferenceRate);
        status_->setText(tr("Captured %1 s — stop and use it, or record again")
                             .arg(seconds, 0, 'f', 1));
        startBtn_->setEnabled(true);
        stopBtn_->setEnabled(true);
    }

    WasapiCapture *capture_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *startBtn_ = nullptr;
    QPushButton *stopBtn_ = nullptr;
    QComboBox *language_ = nullptr;
    QPlainTextEdit *script_ = nullptr;
    QVector<float> samples_;
    QByteArray wavBytes_;
    bool recording_ = false;
    bool draining_ = false;
    static constexpr int kReferenceRate = 24000;
};

} // namespace

SettingsDialog::SettingsDialog(PanelApp *app, QWidget *parent)
    : QDialog(parent), app_(app) {
    buildUi();
    load();
    app_->voices()->refresh();
}

void SettingsDialog::buildUi() {
    setWindowTitle(tr("Voice panel settings"));
    setMinimumWidth(560);
    resize(640, qMin(760, screen()->availableGeometry().height() - 60));

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("settingsPage");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setSpacing(10);

    // ---- connection & modes ----
    auto *connBox = new QGroupBox(tr("Service and processing"));
    auto *connForm = new QFormLayout(connBox);
    serviceUrl_ = new QLineEdit;
    connForm->addRow(tr("Service URL"), serviceUrl_);
    asrModel_ = new QComboBox;
    asrModel_->addItem(tr("Service default"), "default");
    asrModel_->addItem("Qwen3-ASR (fast, RU/EN)", "qwen3-asr");
    asrModel_->addItem("Whisper (timestamps)", "whisper");
    connForm->addRow(tr("Recognition model"), asrModel_);
    autoClean_ = new QCheckBox(tr("Correct completed sentences automatically"));
    autoClean_->setObjectName("autoCorrection");
    connForm->addRow(QString(), autoClean_);
    auto *languageHint = new QLabel(tr("Russian and English are detected automatically."));
    languageHint->setProperty("role", "caption");
    connForm->addRow(languageHint);
    inputDevice_ = new QComboBox;
    for (const auto &[id, name] : WasapiCapture::listDevices(true))
        inputDevice_->addItem(name, id);
    connForm->addRow(tr("Microphone"), inputDevice_);
    hotkey1_ = new QLineEdit;
    hotkey1_->setPlaceholderText("Ctrl+Alt+D");
    connForm->addRow(tr("Dictate hotkey (quick)"), hotkey1_);
    hotkey2_ = new QLineEdit;
    hotkey2_->setPlaceholderText("Ctrl+Alt+R");
    connForm->addRow(tr("Dictate hotkey (review)"), hotkey2_);
    saveHotkey_ = new QLineEdit;
    saveHotkey_->setObjectName("saveHotkey");
    connForm->addRow(tr("Save reviewed text hotkey"), saveHotkey_);
    quickInsert_ = new QCheckBox(tr("Insert text automatically in quick mode"));
    connForm->addRow(QString(), quickInsert_);
    reviewFirst_ = new QCheckBox(tr("Always open the editor before inserting"));
    connForm->addRow(QString(), reviewFirst_);
    dockPosition_ = new QComboBox;
    dockPosition_->setObjectName("dockPosition");
    dockPosition_->addItem(tr("Free / draggable"), "free");
    dockPosition_->addItem(tr("Snap to top"), "top");
    dockPosition_->addItem(tr("Snap to bottom"), "bottom");
    dockPosition_->addItem(tr("Snap to left"), "left");
    dockPosition_->addItem(tr("Snap to right"), "right");
    connForm->addRow(tr("Panel placement"), dockPosition_);
    lay->addWidget(connBox);

    auto *contextBox = new QGroupBox(tr("Your context document"));
    auto *contextLayout = new QVBoxLayout(contextBox);
    auto *contextHint = new QLabel(tr("One document for your topics and vocabulary. It applies to every recording, correction, and translation."));
    contextHint->setWordWrap(true);
    contextHint->setProperty("role", "caption");
    contextLayout->addWidget(contextHint);
    auto *pathRow = new QHBoxLayout;
    contextPath_ = new QLineEdit;
    contextPath_->setReadOnly(true);
    contextPath_->setObjectName("contextDocument");
    pathRow->addWidget(contextPath_, 1);
    auto *chooseContext = new QPushButton(tr("Choose…"));
    pathRow->addWidget(chooseContext);
    contextLayout->addLayout(pathRow);
    context_ = new QPlainTextEdit;
    context_->setObjectName("userContext");
    context_->setMinimumHeight(160);
    context_->setMaximumHeight(220);
    contextLayout->addWidget(context_);
    connect(chooseContext, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Choose your context document"),
            contextPath_->text(), tr("Text documents (*.md *.txt)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        contextPath_->setText(path);
        context_->setPlainText(QString::fromUtf8(file.readAll()));
    });
    lay->addWidget(contextBox);

    // ---- VAD advanced ----
    auto *vadBox = new QGroupBox(tr("Speech detection (advanced)"));
    auto *vadForm = new QFormLayout(vadBox);
    threshold_ = new QDoubleSpinBox;
    threshold_->setRange(0.05, 0.95);
    threshold_->setSingleStep(0.05);
    vadForm->addRow(tr("Speech threshold"), threshold_);
    silenceFrames_ = new QSpinBox;
    silenceFrames_->setRange(4, 120);
    silenceFrames_->setSuffix(tr(" frames"));
    vadForm->addRow(tr("Silence before finalizing (~32 ms each)"), silenceFrames_);
    transcriptReset_ = new QSpinBox;
    transcriptReset_->setObjectName("transcriptReset");
    transcriptReset_->setRange(0, 300);
    transcriptReset_->setSuffix(tr(" s"));
    transcriptReset_->setSpecialValueText(tr("Never"));
    transcriptReset_->setToolTip(tr("After this much silence, a completed quick transcript moves to History and the panes reset. "
                                    "Pending processing finishes first. Auto-save uses the configured folder; insertion or save failures do not block recording. "
                                    "The microphone and the speech detector keep running."));
    vadForm->addRow(tr("Start a new transcript after silence"), transcriptReset_);
    minSpeechFrames_ = new QSpinBox;
    minSpeechFrames_->setRange(1, 60);
    vadForm->addRow(tr("Minimum speech duration"), minSpeechFrames_);
    prerollFrames_ = new QSpinBox;
    prerollFrames_->setRange(0, 50);
    vadForm->addRow(tr("Pre-speech padding"), prerollFrames_);
    maxUtterance_ = new QSpinBox;
    maxUtterance_->setRange(5, 300);
    maxUtterance_->setSuffix(" s");
    vadForm->addRow(tr("Maximum utterance length"), maxUtterance_);
    speculateEvery_ = new QSpinBox;
    speculateEvery_->setRange(10, 400);
    speculateEvery_->setSuffix(tr(" frames"));
    vadForm->addRow(tr("Speculative transcription interval"), speculateEvery_);
    speculative_ = new QCheckBox(tr("Show provisional text while speaking"));
    vadForm->addRow(QString(), speculative_);
    auto *vadRow = new QWidget;
    auto *vadRowLay = new QHBoxLayout(vadRow);
    vadRowLay->setContentsMargins(0, 0, 0, 0);
    vadModel_ = new QLineEdit;
    vadModel_->setPlaceholderText(tr("auto-detect models/silero_vad.onnx next to the app"));
    auto *vadBrowse = new QPushButton(tr("…"));
    connect(vadBrowse, &QPushButton::clicked, this, &SettingsDialog::browseVadModel);
    vadRowLay->addWidget(vadModel_);
    vadRowLay->addWidget(vadBrowse);
    vadForm->addRow(tr("VAD model file"), vadRow);
    restoreDefaults_ = new QPushButton(tr("Restore defaults"));
    connect(restoreDefaults_, &QPushButton::clicked, this, [this]() {
        Settings defaults;
        threshold_->setValue(defaults.speechThreshold);
        silenceFrames_->setValue(defaults.silenceFrames);
        transcriptReset_->setValue(defaults.transcriptResetS);
        minSpeechFrames_->setValue(defaults.minSpeechFrames);
        prerollFrames_->setValue(defaults.prerollFrames);
        maxUtterance_->setValue(defaults.maxUtteranceS);
        speculateEvery_->setValue(defaults.speculateEveryFrames);
        speculative_->setChecked(defaults.speculative);
    });
    vadForm->addRow(QString(), restoreDefaults_);
    lay->addWidget(vadBox);

    // ---- TTS ----
    auto *ttsBox = new QGroupBox(tr("Speech synthesis"));
    auto *ttsForm = new QFormLayout(ttsBox);
    ttsModel_ = new QComboBox;
    ttsModel_->addItem(tr("Auto by language"), "auto");
    ttsModel_->addItem("Kokoro (English)", "kokoro");
    ttsModel_->addItem("Piper ruslan (Russian)", "piper-ru");
    ttsModel_->addItem(tr("Cloned voice (profile below)"), "profile");
    ttsForm->addRow(tr("Model"), ttsModel_);
    ttsRussianVoice_ = new QComboBox;
    ttsRussianVoice_->setObjectName("russianVoice");
    ttsEnglishVoice_ = new QComboBox;
    ttsEnglishVoice_->setObjectName("englishVoice");
    ttsForm->addRow(tr("Russian voice"), ttsRussianVoice_);
    ttsForm->addRow(tr("English voice"), ttsEnglishVoice_);
    auto *voiceHint = new QLabel(tr("Use your recorded voice in both fields, or choose a different reference for each language. Open Voice playback from the tray to test text."));
    voiceHint->setWordWrap(true);
    voiceHint->setProperty("role", "caption");
    ttsForm->addRow(voiceHint);
    connect(ttsRussianVoice_, &QComboBox::currentIndexChanged, this, [this]() { selectedRussianVoice_ = ttsRussianVoice_->currentData().toString(); });
    connect(ttsEnglishVoice_, &QComboBox::currentIndexChanged, this, [this]() { selectedEnglishVoice_ = ttsEnglishVoice_->currentData().toString(); });
    ttsSpeed_ = new QDoubleSpinBox;
    ttsSpeed_->setRange(0.5, 2.0);
    ttsSpeed_->setSingleStep(0.1);
    ttsForm->addRow(tr("Speed"), ttsSpeed_);
    lay->addWidget(ttsBox);

    // ---- voice profiles ----
    auto *voiceBox = new QGroupBox(tr("Voice profiles (zero-shot cloning)"));
    auto *voiceLay = new QVBoxLayout(voiceBox);
    voiceList_ = new QListWidget;
    voiceList_->setMaximumHeight(120);
    voiceLay->addWidget(voiceList_);
    auto *voiceBtns = new QHBoxLayout;
    voiceCreate_ = new QPushButton(tr("New from file…"));
    voiceRecord_ = new QPushButton(tr("Record from mic…"));
    voiceEdit_ = new QPushButton(tr("Edit…"));
    voicePreview_ = new QPushButton(tr("Preview"));
    voiceDelete_ = new QPushButton(tr("Delete"));
    voiceBtns->addWidget(voiceCreate_);
    voiceBtns->addWidget(voiceRecord_);
    voiceBtns->addStretch();
    voiceLay->addLayout(voiceBtns);
    auto *voiceActions = new QHBoxLayout;
    voiceActions->addWidget(voiceEdit_);
    voiceActions->addWidget(voicePreview_);
    voiceActions->addWidget(voiceDelete_);
    voiceActions->addStretch();
    voiceLay->addLayout(voiceActions);
    voiceDelete_->setProperty("variant", "destructive");
    const auto updateVoiceActions = [this]() {
        const bool selected = voiceList_->currentRow() >= 0;
        voiceEdit_->setEnabled(selected);
        voicePreview_->setEnabled(selected);
        voiceDelete_->setEnabled(selected);
    };
    connect(voiceList_, &QListWidget::currentRowChanged, this, updateVoiceActions);
    updateVoiceActions();
    voiceStatus_ = new QLabel;
    voiceStatus_->setWordWrap(true);
    voiceStatus_->setProperty("role", "caption");
    voiceLay->addWidget(voiceStatus_);
    lay->addWidget(voiceBox);

    // ---- storage ----
    auto *storeBox = new QGroupBox(tr("Storage"));
    auto *storeForm = new QFormLayout(storeBox);
    auto *dirRow = new QWidget;
    auto *dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    markdownDir_ = new QLineEdit;
    auto *browse = new QPushButton(tr("…"));
    connect(browse, &QPushButton::clicked, this, &SettingsDialog::browseMarkdownDir);
    dirLay->addWidget(markdownDir_);
    dirLay->addWidget(browse);
    storeForm->addRow(tr("Markdown session folder (empty = off)"), dirRow);
    autoSave_ = new QCheckBox(tr("Save sessions automatically"));
    storeForm->addRow(QString(), autoSave_);
    audioRetention_ = new QSpinBox;
    audioRetention_->setRange(0, 365);
    audioRetention_->setSpecialValueText(tr("keep none"));
    audioRetention_->setSuffix(tr(" days"));
    storeForm->addRow(tr("Keep raw audio"), audioRetention_);
    lay->addWidget(storeBox);

    lay->addStretch(1);
    for (auto *form : page->findChildren<QFormLayout *>()) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(10);
    }
    for (auto *label : page->findChildren<QLabel *>()) label->setWordWrap(true);
    for (auto *combo : page->findChildren<QComboBox *>()) {
        combo->setMinimumWidth(0);
        combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    scroll->setWidget(page);

    auto *rootLay = new QVBoxLayout(this);
    rootLay->addWidget(scroll);
    auto *buttons = new QHBoxLayout;
    auto *ok = new QPushButton(tr("Save"));
    ok->setProperty("variant", "primary");
    auto *cancel = new QPushButton(tr("Cancel"));
    cancel->setProperty("variant", "ghost");
    connect(ok, &QPushButton::clicked, this, &SettingsDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addStretch(1);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    buttons->addWidget(new QSizeGrip(this));
    rootLay->addLayout(buttons);
    UiKit::wrapDialog(*this, windowTitle());

    connect(voiceCreate_, &QPushButton::clicked, this, &SettingsDialog::createVoice);
    connect(voiceRecord_, &QPushButton::clicked, this, &SettingsDialog::recordVoice);
    connect(voiceEdit_, &QPushButton::clicked, this, &SettingsDialog::editVoice);
    connect(voicePreview_, &QPushButton::clicked, this, &SettingsDialog::previewVoice);
    connect(voiceDelete_, &QPushButton::clicked, this, &SettingsDialog::deleteVoice);
    connect(app_->voices(), &VoiceProfiles::listChanged, this, &SettingsDialog::refreshVoices);
    connect(app_->voices(), &VoiceProfiles::created, this, [this](bool ok, const QString &detail) {
        if (!ok) {
            voiceStatus_->setText(tr("Create failed: %1").arg(detail));
            return;
        }
        voiceStatus_->setText(tr("Voice saved. Check that its reference transcript matches the recording."));
        pendingVoiceId_ = detail;
        refreshVoices();
        // The recording exists whether or not this dialog is saved, and the
        // panel has just chosen it for its language: keep that choice, or the
        // new voice would never be used.
        auto *settings = app_->settings();
        settings->ttsVoiceProfile = detail;
        settings->ttsRussianVoice = selectedRussianVoice_;
        settings->ttsEnglishVoice = selectedEnglishVoice_;
        settings->save();
    });
    connect(app_->voices(), &VoiceProfiles::updated, this, [this](bool ok, const QString &detail) {
        voiceStatus_->setText(ok ? tr("Profile updated.") : tr("Update failed: %1").arg(detail));
    });
    connect(app_->voices(), &VoiceProfiles::removed, this, [this](bool ok) {
        if (ok) voiceStatus_->setText(tr("Profile deleted."));
    });
    connect(app_->voices(), &VoiceProfiles::previewReady, this,
            [this](bool ok, const QString &pathOrError) {
                if (!ok) {
                    voiceStatus_->setText(tr("Preview failed: %1").arg(pathOrError));
                    return;
                }
                voiceStatus_->setText(tr("Playing preview…"));
                PlaySoundW(reinterpret_cast<LPCWSTR>(pathOrError.utf16()), nullptr,
                           SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            });
}

void SettingsDialog::load() {
    auto *s = app_->settings();
    selectedRussianVoice_ = s->ttsRussianVoice;
    selectedEnglishVoice_ = s->ttsEnglishVoice;
    serviceUrl_->setText(s->serviceUrl);
    asrModel_->setCurrentIndex(asrModel_->findData(s->asrModel));
    contextPath_->setText(s->contextDocument);
    context_->setPlainText(s->context);
    autoClean_->setChecked(s->autoCleanEditor);
    if (int i = inputDevice_->findData(s->inputDevice); i >= 0) inputDevice_->setCurrentIndex(i);
    hotkey1_->setText(s->insertHotkey);
    hotkey2_->setText(s->insertHotkey2);
    saveHotkey_->setText(s->saveHotkey);
    dockPosition_->setCurrentIndex(qMax(0, dockPosition_->findData(s->dockPosition)));
    quickInsert_->setChecked(s->quickInsert);
    reviewFirst_->setChecked(s->reviewBeforeInsert);

    threshold_->setValue(s->speechThreshold);
    silenceFrames_->setValue(s->silenceFrames);
    transcriptReset_->setValue(s->transcriptResetS);
    minSpeechFrames_->setValue(s->minSpeechFrames);
    prerollFrames_->setValue(s->prerollFrames);
    maxUtterance_->setValue(s->maxUtteranceS);
    speculateEvery_->setValue(s->speculateEveryFrames);
    speculative_->setChecked(s->speculative);

    ttsSpeed_->setValue(s->ttsSpeed);
    if (!s->ttsModel.isEmpty() && ttsModel_->findData(s->ttsModel) < 0)
        ttsModel_->addItem(s->ttsModel, s->ttsModel);
    ttsModel_->setCurrentIndex(qMax(0, ttsModel_->findData(s->ttsModel)));
    vadModel_->setText(s->vadModel);
    markdownDir_->setText(s->transcriptDirectory());
    autoSave_->setChecked(s->autoSaveSessions);
    audioRetention_->setValue(s->audioRetentionDays);
    refreshVoices();
    loadServiceModels();
}

void SettingsDialog::loadServiceModels() {
    QPointer<SettingsDialog> self(this);
    app_->service()->getJson("/v1/models", [this, self](bool ok, QByteArray body, QString) {
        if (!self || !ok) return;
        const auto arr = QJsonDocument::fromJson(body).object().value("data").toArray();
        QStringList asr, tts;
        for (const auto &v : arr) {
            const auto o = v.toObject();
            const QString id = o.value("id").toString();
            const QString cap = o.value("capability").toString();
            if (id.isEmpty()) continue;
            if (cap == "asr") asr << id;
            else if (cap == "tts") tts << id;
        }
        const auto label = [](const QString &id) {
            if (id == "qwen3-asr") return QStringLiteral("Qwen3-ASR (fast, RU/EN)");
            if (id == "whisper") return QStringLiteral("Whisper (timestamps)");
            if (id == "kokoro") return QStringLiteral("Kokoro (English)");
            if (id == "piper-ru") return QStringLiteral("Piper ruslan (Russian)");
            if (id == "qwen3-tts-clone") return QStringLiteral("Qwen3-TTS clone (RU/EN)");
            if (id == "chatterbox") return QStringLiteral("Chatterbox clone (multilingual)");
            return id;
        };
        if (!asr.isEmpty()) {
            const QString keep = asrModel_->currentData().toString();
            asrModel_->clear();
            asrModel_->addItem(tr("Service default"), "default");
            for (const QString &id : asr) asrModel_->addItem(label(id), id);
            const int idx = asrModel_->findData(keep);
            asrModel_->setCurrentIndex(idx >= 0 ? idx : 0);
        }
        if (!tts.isEmpty()) {
            const QString keep = ttsModel_->currentData().toString();
            ttsModel_->clear();
            ttsModel_->addItem(tr("Auto by language"), "auto");
            for (const QString &id : tts) ttsModel_->addItem(label(id), id);
            ttsModel_->addItem(tr("Cloned voice (profile below)"), "profile");
            const int idx = ttsModel_->findData(keep);
            ttsModel_->setCurrentIndex(idx >= 0 ? idx : 0);
        }
    });
}

void SettingsDialog::refreshVoices() {
    voiceList_->clear();
    for (const auto &p : app_->voices()->profiles()) {
        voiceList_->addItem(QString("%1 (%2, %3 s)").arg(p.name, p.language)
                                .arg(p.referenceDurationS, 0, 'f', 1));
        if (p.id == pendingVoiceId_) {
            if (selectedRussianVoice_.isEmpty() && selectedEnglishVoice_.isEmpty())
                selectedRussianVoice_ = selectedEnglishVoice_ = p.id;
            else if (pendingVoiceLanguage_ == "ru") selectedRussianVoice_ = p.id;
            else selectedEnglishVoice_ = p.id;
            pendingVoiceId_.clear();
        }
    }
    const auto populate = [this](QComboBox *combo, const QString &selected, bool russian) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(russian ? tr("Built-in voice (English only)") : tr("Built-in English voice"), "builtin");
        for (const auto &voice : app_->voices()->profiles()) combo->addItem(voice.name, voice.id);
        if (!selected.isEmpty() && combo->findData(selected) < 0)
            combo->addItem(tr("Saved voice (currently unavailable)"), selected);
        combo->setCurrentIndex(qMax(0, combo->findData(selected)));
    };
    populate(ttsRussianVoice_, selectedRussianVoice_, true);
    populate(ttsEnglishVoice_, selectedEnglishVoice_, false);
}

void SettingsDialog::createVoice() {
    const QString wav = QFileDialog::getOpenFileName(
        this, tr("Choose a reference recording (5-20 s of one voice)"), {},
        tr("Audio (*.wav *.mp3 *.flac *.ogg)"));
    if (wav.isEmpty()) return;
    bool okName = false;
    const QString name = QInputDialog::getText(this, tr("Voice profile name"),
                                               tr("Name:"), QLineEdit::Normal, QFileInfo(wav).baseName(),
                                               &okName);
    if (!okName || name.trimmed().isEmpty()) return;
    bool okLang = false;
    const QString lang = QInputDialog::getItem(this, tr("Reference language"),
                                               tr("Language of the recording:"),
                                               {"ru", "en"}, 0, false, &okLang);
    if (!okLang) return;
    pendingVoiceLanguage_ = lang;
    app_->voices()->create(name, wav, {}, lang);   // transcript auto-drafted by the service ASR
}

void SettingsDialog::recordVoice() {
    app_->tts()->stop();
    PlaySoundW(nullptr, nullptr, 0);
    RecordVoiceDialog dlg(inputDevice_->currentData().toString(), this);
    if (dlg.exec() != QDialog::Accepted || dlg.wavBytes().isEmpty()) return;
    bool okName = false;
    const QString name = QInputDialog::getText(this, tr("Voice profile name"), tr("Name:"),
                                               QLineEdit::Normal, tr("My voice"), &okName);
    if (!okName || name.trimmed().isEmpty()) return;
    const QString lang = dlg.language();
    pendingVoiceLanguage_ = lang;
    const QString path = QDir::temp().filePath(
        QString("voicepanel-ref-%1.wav").arg(QDateTime::currentMSecsSinceEpoch()));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Voice profile"), tr("Cannot write %1").arg(path));
        return;
    }
    f.write(dlg.wavBytes());
    f.close();
    voiceStatus_->setText(tr("Uploading the reference…"));
    app_->voices()->create(name, path, dlg.referenceText(), lang);
    QFile::remove(path);
}

void SettingsDialog::editVoice() {
    const auto profiles = app_->voices()->profiles();
    const int row = voiceList_->currentRow();
    if (row < 0 || row >= profiles.size()) return;
    const VoiceProfile p = profiles[row];

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit voice profile"));
    dlg.setMinimumWidth(480);
    auto *lay = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(p.name, &dlg);
    form->addRow(tr("Name"), name);
    auto *lang = new QComboBox(&dlg);
    lang->addItem("Russian (ru)", "ru");
    lang->addItem("English (en)", "en");
    if (lang->findData(p.language) >= 0) lang->setCurrentIndex(lang->findData(p.language));
    form->addRow(tr("Reference language"), lang);
    auto *text = new QPlainTextEdit(p.referenceText, &dlg);
    text->setFixedHeight(90);
    form->addRow(tr("Reference transcript"), text);
    lay->addLayout(form);
    auto *hint = new QLabel(tr("The transcript must match the reference recording word for word; "
                               "it improves cloning quality on the Base model."), &dlg);
    hint->setWordWrap(true);
    hint->setProperty("role", "caption");
    lay->addWidget(hint);
    auto *buttons = new QHBoxLayout;
    auto *ok = new QPushButton(tr("Save"), &dlg);
    ok->setProperty("variant", "primary");
    auto *cancel = new QPushButton(tr("Cancel"), &dlg);
    buttons->addStretch(1);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    lay->addLayout(buttons);
    connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    UiKit::wrapDialog(dlg, dlg.windowTitle());
    if (dlg.exec() != QDialog::Accepted) return;
    app_->voices()->update(p.id, name->text(), text->toPlainText(), lang->currentData().toString());
}

void SettingsDialog::browseVadModel() {
    const QString f = QFileDialog::getOpenFileName(this, tr("Silero VAD model"), {},
                                                   tr("ONNX model (*.onnx)"));
    if (!f.isEmpty()) vadModel_->setText(f);
}

void SettingsDialog::previewVoice() {
    const auto profiles = app_->voices()->profiles();
    const int row = voiceList_->currentRow();
    if (row < 0 || row >= profiles.size()) return;
    const QString id = profiles[row].id;
    app_->voices()->preview(id);
}

void SettingsDialog::deleteVoice() {
    const auto profiles = app_->voices()->profiles();
    const int row = voiceList_->currentRow();
    if (row < 0 || row >= profiles.size()) return;
    if (QMessageBox::question(this, tr("Delete voice profile"),
                              tr("Delete '%1'?").arg(profiles[row].name)) != QMessageBox::Yes)
        return;
    app_->voices()->remove(profiles[row].id);
}

void SettingsDialog::browseMarkdownDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Markdown output folder"));
    if (!dir.isEmpty()) markdownDir_->setText(dir);
}

void SettingsDialog::accept() {
    QStringList shortcuts;
    for (auto *field : {hotkey1_, hotkey2_, saveHotkey_}) {
        const QString shortcut = field->text().trimmed().toLower();
        if (!HotkeyManager::validCombo(shortcut) || shortcuts.contains(shortcut)) {
            QMessageBox::warning(this, tr("Global hotkeys"), tr("Use different key combinations, each with a modifier."));
            return;
        }
        shortcuts << shortcut;
    }
    const QUrl serviceUrl(serviceUrl_->text().trimmed());
    if (!serviceUrl.isValid() || serviceUrl.host().isEmpty() ||
        (serviceUrl.scheme() != "http" && serviceUrl.scheme() != "https")) {
        QMessageBox::warning(this, tr("Service URL"), tr("Enter an HTTP or HTTPS service address."));
        return;
    }
    auto *s = app_->settings();
    if (contextPath_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Context document"), tr("Choose a file for your context document."));
        return;
    }
    const QString oldPath = s->contextDocument, oldContext = s->context;
    s->contextDocument = contextPath_->text().trimmed();
    s->context = context_->toPlainText();
    QString contextError;
    if (!s->saveContext(&contextError)) {
        s->contextDocument = oldPath;
        s->context = oldContext;
        QMessageBox::warning(this, tr("Context document"), contextError);
        return;
    }
    s->language = "auto";
    s->serviceUrl = serviceUrl_->text().trimmed();
    app_->service()->setBaseUrl(s->serviceUrl);
    s->asrModel = asrModel_->currentData().toString();
    s->profile = "general";
    s->autoCleanEditor = autoClean_->isChecked();
    s->inputDevice = inputDevice_->currentData().toString();
    s->insertHotkey = hotkey1_->text();
    s->insertHotkey2 = hotkey2_->text();
    s->saveHotkey = saveHotkey_->text().trimmed();
    s->dockPosition = dockPosition_->currentData().toString();
    s->quickInsert = quickInsert_->isChecked();
    s->reviewBeforeInsert = reviewFirst_->isChecked();

    s->speechThreshold = threshold_->value();
    s->silenceFrames = silenceFrames_->value();
    s->transcriptResetS = transcriptReset_->value();
    s->minSpeechFrames = minSpeechFrames_->value();
    s->prerollFrames = prerollFrames_->value();
    s->maxUtteranceS = maxUtterance_->value();
    s->speculateEveryFrames = speculateEvery_->value();
    s->speculative = speculative_->isChecked();
    s->vadModel = vadModel_->text().trimmed();

    s->ttsModel = ttsModel_->currentData().toString();
    s->ttsRussianVoice = selectedRussianVoice_;
    s->ttsEnglishVoice = selectedEnglishVoice_;
    s->ttsSpeed = ttsSpeed_->value();
    s->markdownDir = markdownDir_->text();
    s->autoSaveSessions = autoSave_->isChecked();
    s->audioRetentionDays = audioRetention_->value();
    s->save();
    const QString hotkeyError = app_->applySettings();
    if (!hotkeyError.isEmpty()) QMessageBox::warning(this, tr("Global hotkeys"), hotkeyError);
    QDialog::accept();
}
