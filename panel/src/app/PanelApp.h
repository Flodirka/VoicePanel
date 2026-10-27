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

// Application bootstrap: settings, service client, pipeline wiring, tray.
class PanelApp : public QApplication {
    Q_OBJECT
public:
    PanelApp(int &argc, char **argv);
    ~PanelApp() override;
    int run();
    QString applySettings();

    Settings *settings() const { return settings_.get(); }
    ServiceClient *service() const { return service_.get(); }
    DictationController *dictation() const { return dictation_.get(); }
    TextProcessor *processor() const { return processor_.get(); }
    HotkeyManager *hotkeys() const { return hotkeys_.get(); }
    TtsPlayer *tts() const { return tts_.get(); }
    MarkdownStore *store() const { return store_.get(); }
    VoiceProfiles *voices() const { return voices_.get(); }
    MainWindow *window() const { return window_; }

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
    QSystemTrayIcon *tray_ = nullptr;
    ::QLocalServer *singleServer_ = nullptr;
};

} // namespace VoicePanel
