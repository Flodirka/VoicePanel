#include "app/VoicePlaybackDialog.h"
#include "app/PanelApp.h"
#include "app/UiKit.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace VoicePanel;

namespace {
// "builtin" is the explicit choice of the engine's own voice; an empty entry
// reaches the player as no voice at all.
QString voiceId(const QString &selection) {
    return selection == "builtin" ? QString() : selection;
}
} // namespace

VoicePlaybackDialog::VoicePlaybackDialog(PanelApp *app, QWidget *parent) : QDialog(parent), app_(app) {
    setWindowTitle(tr("Voice playback"));
    setObjectName("voicePlayback");
    setMinimumSize(440, 360);
    resize(520, 400);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 20);
    layout->setSpacing(12);
    auto *hint = new QLabel(tr("Type or paste text. The Russian or English voice from Settings is selected automatically."));
    hint->setProperty("role", "caption");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *text = new QPlainTextEdit;
    text->setObjectName("speechText");
    text->setPlaceholderText(tr("Paste an agent reply or enter a speech test here."));
    text->setPlainText(tr("This is a test of my selected voice."));
    layout->addWidget(text, 1);
    auto *status = new QLabel(tr("Ready"));
    status->setObjectName("speechStatus");
    status->setWordWrap(true);
    status->setProperty("role", "caption");
    layout->addWidget(status);
    auto *pauseRow = new QHBoxLayout;
    auto *pauseLabel = new QLabel(tr("Pause between sentences"));
    pauseRow->addWidget(pauseLabel);
    auto *pause = new QSpinBox;
    pause->setObjectName("speechPause");
    pause->setRange(0, 1500);
    pause->setSingleStep(50);
    pause->setSuffix(tr(" ms"));
    pause->setValue(app_->settings()->ttsPauseMs);
    pause->setToolTip(tr("Silence added between sentences. 0 keeps only the pause the voice makes."));
    pauseRow->addWidget(pause);
    pauseRow->addStretch(1);
    layout->addLayout(pauseRow);
    connect(pause, &QSpinBox::valueChanged, this, [this](int value) {
        app_->settings()->ttsPauseMs = value;
        app_->settings()->save();
        app_->tts()->setPauseMs(value);
    });
    auto *buttons = new QHBoxLayout;
    auto *play = new QPushButton(app_->tts()->speaking() ? tr("Stop") : tr("Speak"));
    play->setObjectName("speechToggle");
    play->setProperty("variant", "primary");
    buttons->addWidget(play);
    auto *save = new QPushButton(tr("Save audio…"));
    save->setObjectName("speechSave");
    save->setToolTip(tr("Write the spoken text to a WAV file instead of only playing it."));
    buttons->addWidget(save);
    layout->addLayout(buttons);

    connect(save, &QPushButton::clicked, this, [this, text, status, save]() {
        if (text->toPlainText().trimmed().isEmpty()) return;
        auto *settings = app_->settings();
        const QString folder = settings->markdownDir.isEmpty()
                                   ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation)
                                   : settings->markdownDir;
        const QString suggested = QDir(folder).filePath(
            QStringLiteral("speech-%1.wav").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
        const QString path = QFileDialog::getSaveFileName(this, tr("Save speech"), suggested,
                                                          tr("WAV audio (*.wav)"));
        if (path.isEmpty()) return;
        app_->tts()->stop();
        app_->tts()->setModel(settings->ttsModel);
        app_->tts()->setLanguageVoices(voiceId(settings->ttsRussianVoice), voiceId(settings->ttsEnglishVoice));
        app_->tts()->setSpeed(settings->ttsSpeed);
        save->setEnabled(false);
        status->setText(tr("Saving audio…"));
        app_->tts()->saveSpeech(text->toPlainText(), path);
    });
    connect(app_->tts(), &TtsPlayer::saved, this, [status, save](const QString &path) {
        save->setEnabled(true);
        status->setText(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
    });
    connect(app_->tts(), &TtsPlayer::saveFailed, this, [status, save](const QString &message) {
        save->setEnabled(true);
        status->setText(message);
    });
    connect(play, &QPushButton::clicked, this, [this, text, status, play, pause]() {
        if (app_->tts()->speaking()) {
            app_->tts()->stop();
            return;
        }
        if (text->toPlainText().trimmed().isEmpty()) return;
        auto *settings = app_->settings();
        app_->tts()->setModel(settings->ttsModel);
        app_->tts()->setLanguageVoices(voiceId(settings->ttsRussianVoice),
                                       voiceId(settings->ttsEnglishVoice));
        app_->tts()->setSpeed(settings->ttsSpeed);
        app_->tts()->setPauseMs(pause->value());
        status->setText(tr("Preparing speech…"));
        play->setText(tr("Stop"));
        app_->tts()->speak(text->toPlainText());
    });
    connect(app_->tts(), &TtsPlayer::started, this, [status]() { status->setText(tr("Playing…")); });
    connect(app_->tts(), &TtsPlayer::finished, this, [status, play]() { status->setText(tr("Ready")); play->setText(tr("Speak")); });
    connect(app_->tts(), &TtsPlayer::error, this, [status, play](const QString &error) { status->setText(error); play->setText(tr("Speak")); });
    UiKit::wrapDialog(*this, windowTitle());
}

VoicePlaybackDialog::~VoicePlaybackDialog() { app_->tts()->stop(); }
