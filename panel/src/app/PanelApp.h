#pragma once
#include <QApplication>
#include <memory>

class QSystemTrayIcon;
class QLocalServer;
class PanelTests;

namespace VoicePanel {

class MainWindow;
struct Settings;
class ServiceClient;
class DictationController;
class TextProcessor;
class HotkeyManager;
class TtsPlayer;
class MarkdownStore;
class VoiceProfiles;
class SessionWindow;
class CaptionOverlay;
class MeetingWindow;

// Application bootstrap: settings, service client, pipeline wiring, tray.
class PanelApp : public QApplication {
    Q_OBJECT
public:
    PanelApp(int &argc, char **argv);
    ~PanelApp() override;
    int run();
    QString applySettings();
    // Headless bundle self-test (--check-bundle): prove the packaged app's
    // parts resolve — settings, context document, terminology dictionary,
    // VAD model, hotkeys, service /health — without a window, tray, message
    // loop or microphone. Returns 0 when every check passes.
    int runCheckBundle();

    Settings *settings() const { return settings_.get(); }
    ServiceClient *service() const { return service_.get(); }
    DictationController *dictation() const { return dictation_.get(); }
    TextProcessor *processor() const { return processor_.get(); }
    HotkeyManager *hotkeys() const { return hotkeys_.get(); }
    TtsPlayer *tts() const { return tts_.get(); }
    MarkdownStore *store() const { return store_.get(); }
    VoiceProfiles *voices() const { return voices_.get(); }
    MainWindow *window() const { return window_; }

    // 0 = interpreter, 1 = meeting. Creates the window on first use and
    // starts listening immediately.
    void openSession(int mode);
    // Compact always-on-top captions for one application (a video, a stream).
    void openSubtitles();
    QString vadPath() const { return vadPath_; }

private:
    friend class ::PanelTests;
    void ensureSingleInstance();
    void wireHotkeys();
    void loadVadModel();

    std::unique_ptr<Settings> settings_;
    std::unique_ptr<ServiceClient> service_;
    std::unique_ptr<DictationController> dictation_;
    std::unique_ptr<TextProcessor> processor_;
    std::unique_ptr<HotkeyManager> hotkeys_;
    std::unique_ptr<TtsPlayer> tts_;
    std::unique_ptr<MarkdownStore> store_;
    std::unique_ptr<VoiceProfiles> voices_;
    MainWindow *window_ = nullptr;
    SessionWindow *sessionWindow_ = nullptr;
    CaptionOverlay *overlay_ = nullptr;
    MeetingWindow *meetingWindow_ = nullptr;
    QString vadPath_;
    QSystemTrayIcon *tray_ = nullptr;
    ::QLocalServer *singleServer_ = nullptr;
};

} // namespace VoicePanel
