#include "app/VoicePlaybackDialog.h"
#include "app/PanelApp.h"
#include "app/UiKit.h"
#include "core/LiveSession.h"
#include "core/ServiceClient.h"
#include "core/Settings.h"
#include "core/TtsPlayer.h"

#include <QComboBox>
#include <QDataStream>
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

QByteArray encodeWav(const QVector<float> &samples) {
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataSize = quint32(samples.size()) * 2;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataSize);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(16000) << quint32(32000)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << dataSize;
    for (float value : samples) {
        out << qint16(qBound(-1.0, double(value), 1.0) * 32767.0);
    }
    return wav;
}
} // namespace

VoicePlaybackDialog::VoicePlaybackDialog(PanelApp *app, QWidget *parent) : QDialog(parent), app_(app) {
    setWindowTitle(tr("Voice"));
    setObjectName("voicePlayback");
    setWindowIcon(UiKit::icon("mic"));
    setMinimumSize(480, 420);
    resize(560, 460);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 20);
    layout->setSpacing(12);

    auto *hint = new QLabel(tr("Type, paste or dictate text, then speak it or save it. Choose a "
                               "virtual cable under “Play into” to record a voice message into "
                               "another application."));
    hint->setProperty("role", "caption");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *text = new QPlainTextEdit;
    text->setObjectName("speechText");
    text->setPlaceholderText(tr("Paste an agent reply, or dictate and review before speaking."));
    text->setPlainText(tr("This is a test of my selected voice."));
    layout->addWidget(text, 1);

    auto *status = new QLabel(tr("Ready"));
    status->setObjectName("speechStatus");
    status->setWordWrap(true);
    status->setProperty("role", "caption");
    layout->addWidget(status);

    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(new QLabel(tr("Play into")));
    auto *outputBox = new QComboBox;
    outputBox->setObjectName("speechOutput");
    for (const auto &device : TtsPlayer::listOutputDevices()) {
        outputBox->addItem(device.second, device.first);
    }
    if (int i = outputBox->findData(app_->settings()->outputDevice); i >= 0) {
        outputBox->setCurrentIndex(i);
    }
    outputBox->setToolTip(tr("Speakers for a normal test; a virtual cable when the speech should "
                             "arrive in a call or a recorder."));
    outputRow->addWidget(outputBox, 1);
    layout->addLayout(outputRow);

    auto *pauseRow = new QHBoxLayout;
    pauseRow->addWidget(new QLabel(tr("Pause between sentences")));
    auto *pause = new QSpinBox;
    pause->setObjectName("speechPause");
    pause->setRange(0, 1500);
    pause->setSingleStep(50);
    pause->setSuffix(tr(" ms"));
    pause->setValue(app_->settings()->ttsPauseMs);
    pauseRow->addWidget(pause);
    pauseRow->addStretch(1);
    layout->addLayout(pauseRow);
    connect(pause, &QSpinBox::valueChanged, this, [this](int value) {
        app_->settings()->ttsPauseMs = value;
        app_->settings()->save();
        app_->tts()->setPauseMs(value);
    });

    auto *buttons = new QHBoxLayout;
    auto *dictate = new QPushButton(tr("Dictate"));
    dictate->setObjectName("speechDictate");
    dictate->setIcon(UiKit::icon("mic"));
    dictate->setToolTip(tr("Speak instead of typing; the box above receives the text."));
    buttons->addWidget(dictate);
    auto *play = new QPushButton(app_->tts()->speaking() ? tr("Stop") : tr("Speak"));
    play->setObjectName("speechToggle");
    play->setProperty("variant", "primary");
    buttons->addWidget(play);
    auto *translate = new QPushButton(tr("Translate"));
    translate->setObjectName("speechTranslate");
    translate->setProperty("variant", "ghost");
    translate->setToolTip(tr("Translate the text (Russian to English or back) before speaking or saving."));
    buttons->addWidget(translate);
    auto *save = new QPushButton(tr("Save audio…"));
    save->setObjectName("speechSave");
    save->setToolTip(tr("Write the spoken text to a WAV file instead of only playing it."));
    buttons->addWidget(save);
    layout->addLayout(buttons);

    // ---- dictation into the box -----------------------------------------
    auto *session = new LiveSession(this);
    connect(dictate, &QPushButton::clicked, this, [this, dictate, status, session] {
        if (session->active()) {
            session->stop();
            dictate->setText(tr("Dictate"));
            status->setText(tr("Ready"));
            return;
        }
        QString vadPath = app_->vadPath();
        if (vadPath.isEmpty()) vadPath = app_->settings()->vadModel;
        if (vadPath.isEmpty()) vadPath = QStringLiteral("models/silero_vad.onnx");
        session->setVadModel(vadPath);
        session->setDevice(app_->settings()->inputDevice);
        session->configure(app_->settings()->silenceFrames, app_->settings()->minSpeechFrames,
                           app_->settings()->prerollFrames, 20);
        if (!session->start()) {
            status->setText(tr("Could not open the microphone"));
            return;
        }
        dictate->setText(tr("Stop"));
        status->setText(tr("Listening…"));
    });
    connect(session, &LiveSession::error, this, [status](const QString &message) {
        status->setText(message);
    });
    connect(session, &LiveSession::utterance, this,
            [this, dictate, status, text, session](const QVector<float> &samples, double) {
        session->stop();
        dictate->setText(tr("Dictate"));
        status->setText(tr("Transcribing…"));
        const QString model = app_->settings()->asrModel == QLatin1String("default")
                                  ? QString()
                                  : app_->settings()->asrModel;
        app_->service()->transcribe(
            encodeWav(samples), model, QString(),
            [status, text](bool ok, QString result, QString error) {
                if (!ok) {
                    status->setText(error);
                    return;
                }
                const QString trimmed = result.trimmed();
                if (trimmed.isEmpty()) {
                    status->setText(QObject::tr("Nothing recognised"));
                    return;
                }
                const QString current = text->toPlainText();
                text->setPlainText(current.isEmpty() ? trimmed
                                                     : current + QLatin1Char(' ') + trimmed);
                status->setText(QObject::tr("Added to the text"));
            });
    });

    // ---- speak / save ----------------------------------------------------
    connect(translate, &QPushButton::clicked, this, [this, text, status, translate] {
        const QString source = text->toPlainText().trimmed();
        if (source.isEmpty()) return;
        bool cyrillic = false;
        for (const QChar ch : source) {
            if (ch.unicode() >= 0x0400 && ch.unicode() <= 0x04FF) {
                cyrillic = true;
                break;
            }
        }
        const QString mode = cyrillic ? QStringLiteral("translate_ru_en")
                                      : QStringLiteral("translate_en_ru");
        translate->setEnabled(false);
        status->setText(cyrillic ? tr("Translating into English…") : tr("Translating into Russian…"));
        app_->service()->processText(
            source, mode, app_->settings()->profile, QString(), {},
            [status, text, translate, cyrillic](bool ok, QString result, QString, QString error) {
                translate->setEnabled(true);
                if (!ok) {
                    status->setText(error);
                    return;
                }
                text->setPlainText(result.trimmed());
                status->setText(cyrillic ? QObject::tr("Translated into English")
                                         : QObject::tr("Translated into Russian"));
            });
    });
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
    connect(play, &QPushButton::clicked, this, [this, text, status, play, pause, outputBox]() {
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
        const int device = outputBox->currentData().toInt();
        app_->tts()->setOutputDevice(device);
        settings->outputDevice = device;
        settings->save();
        app_->tts()->stop();          // never queue a second clip behind the first
        status->setText(tr("Preparing speech…"));
        play->setText(tr("Stop"));
        app_->tts()->speak(text->toPlainText());
    });
    connect(app_->tts(), &TtsPlayer::started, this, [status]() { status->setText(tr("Playing…")); });
    connect(app_->tts(), &TtsPlayer::finished, this, [status, play]() {
        status->setText(tr("Ready"));
        play->setText(tr("Speak"));
    });
    connect(app_->tts(), &TtsPlayer::error, this, [status, play](const QString &error) {
        status->setText(error);
        play->setText(tr("Speak"));
    });

    UiKit::wrapDialog(*this, windowTitle());
}

VoicePlaybackDialog::~VoicePlaybackDialog() { app_->tts()->stop(); }
