#pragma once
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QSpinBox;
class QLineEdit;
class QSlider;
class QLabel;
class QPushButton;
class QListWidget;
class QPlainTextEdit;

namespace VoicePanel {

class PanelApp;

// Settings UI: simple defaults up front, advanced VAD controls in a collapsible
// group, plus voice profile management (create from recording/file, preview,
// delete) and storage options.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(PanelApp *app, QWidget *parent = nullptr);

private slots:
    void accept() override;
    void refreshVoices();
    void createVoice();
    void recordVoice();
    void editVoice();
    void previewVoice();
    void deleteVoice();
    void browseMarkdownDir();
    void browseVadModel();

private:
    void buildUi();
    void load();
    void loadServiceModels();
    PanelApp *app_;

    // service / modes
    QLineEdit *serviceUrl_ = nullptr;
    QComboBox *asrModel_ = nullptr;
    QLineEdit *contextPath_ = nullptr;
    QPlainTextEdit *context_ = nullptr;
    QCheckBox *autoClean_ = nullptr;
    QComboBox *inputDevice_ = nullptr;
    QLineEdit *hotkey1_ = nullptr;
    QLineEdit *hotkey2_ = nullptr;
    QLineEdit *saveHotkey_ = nullptr;
    QComboBox *dockPosition_ = nullptr;
    QCheckBox *quickInsert_ = nullptr;
    QCheckBox *reviewFirst_ = nullptr;

    // VAD advanced
    QDoubleSpinBox *threshold_ = nullptr;
    QSpinBox *silenceFrames_ = nullptr;
    QSpinBox *transcriptReset_ = nullptr;
    QSpinBox *minSpeechFrames_ = nullptr;
    QSpinBox *prerollFrames_ = nullptr;
    QSpinBox *maxUtterance_ = nullptr;
    QSpinBox *speculateEvery_ = nullptr;
    QCheckBox *speculative_ = nullptr;
    QPushButton *restoreDefaults_ = nullptr;
    QLineEdit *vadModel_ = nullptr;

    // TTS
    QComboBox *ttsModel_ = nullptr;
    QComboBox *ttsRussianVoice_ = nullptr;
    QComboBox *ttsEnglishVoice_ = nullptr;
    QString selectedRussianVoice_;
    QString selectedEnglishVoice_;
    QString pendingVoiceLanguage_;
    QString pendingVoiceId_;
    QDoubleSpinBox *ttsSpeed_ = nullptr;

    // storage
    QLineEdit *markdownDir_ = nullptr;
    QCheckBox *autoSave_ = nullptr;
    QSpinBox *audioRetention_ = nullptr;

    // voice profiles
    QListWidget *voiceList_ = nullptr;
    QPushButton *voiceCreate_ = nullptr;
    QPushButton *voiceRecord_ = nullptr;
    QPushButton *voiceEdit_ = nullptr;
    QPushButton *voicePreview_ = nullptr;
    QPushButton *voiceDelete_ = nullptr;
    QLabel *voiceStatus_ = nullptr;
};

} // namespace VoicePanel
