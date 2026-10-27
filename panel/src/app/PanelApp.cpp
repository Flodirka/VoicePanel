#include "app/PanelApp.h"

#include "app/MainWindow.h"
#include "app/UiKit.h"
#include "core/DictationController.h"
#include "core/HotkeyManager.h"
#include "core/MarkdownStore.h"
#include "core/ServiceClient.h"
#include "core/Settings.h"
#include "core/TextProcessor.h"
#include "core/TtsPlayer.h"
#include "core/VoiceProfiles.h"
#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>

using namespace VoicePanel;

PanelApp::PanelApp(int &argc, char **argv) : QApplication(argc, argv) {
    setApplicationName("VoicePanel");
    setOrganizationName("VoicePanel");
    setQuitOnLastWindowClosed(false);
    UiKit::applyTheme();

    settings_ = std::make_unique<Settings>();
    settings_->load();
    service_ = std::make_unique<ServiceClient>();
    service_->setBaseUrl(settings_->serviceUrl);
    service_->setContext(settings_->context);
    if (!QFile::exists(settings_->contextDocument)) settings_->saveContext();
    dictation_ = std::make_unique<DictationController>(service_.get());
    processor_ = std::make_unique<TextProcessor>(service_.get());
    hotkeys_ = std::make_unique<HotkeyManager>();
    tts_ = std::make_unique<TtsPlayer>(service_.get());
    store_ = std::make_unique<MarkdownStore>();
    voices_ = std::make_unique<VoiceProfiles>(service_.get());
    loadVadModel();
    store_->setDir(settings_->transcriptDirectory());
}

void PanelApp::loadVadModel() {
    // VAD model: explicit setting first, then standard locations next to the
    // application (portable layout) and the per-user data directory. No build-tree
    // walk-up: a packaged app only needs models/ beside its executable.
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        settings_->vadModel,
        appDir + "/models/silero_vad.onnx",
        appDir + "/runtime/models/silero_vad.onnx",
        appDir + "/../models/silero_vad.onnx",
        appData + "/models/silero_vad.onnx",
    };
    for (const QString &c : candidates) {
        if (!c.isEmpty() && QFile::exists(c)) {
            dictation_->setVadModel(c);
            break;
        }
    }
}

PanelApp::~PanelApp() { delete window_; }

void PanelApp::ensureSingleInstance() {
    QLocalSocket probe;
    probe.connectToServer("voicepanel-single");
    if (probe.waitForConnected(200)) {
        probe.write("activate");
        probe.flush();
        probe.disconnectFromServer();
        ::exit(0);
    }
    QLocalServer::removeServer("voicepanel-single");
    singleServer_ = new QLocalServer(this);
    singleServer_->listen("voicepanel-single");
    connect(singleServer_, &QLocalServer::newConnection, this, [this]() {
        if (auto *c = singleServer_->nextPendingConnection()) {
            c->deleteLater();
            if (window_) {
                window_->showExpanded();
                window_->raise();
                window_->activateWindow();
            }
        }
    });
}

void PanelApp::wireHotkeys() {
    connect(hotkeys_.get(), &HotkeyManager::triggered, window_, [this](const QString &combo) {
        if (combo == settings_->insertHotkey) {
            window_->toggleDictation(false);
        } else if (combo == settings_->insertHotkey2) {
            window_->toggleDictation(true);
        } else if (combo == settings_->saveHotkey) {
            window_->saveReviewedText();
        }
    });
}

QString PanelApp::applySettings() {
    service_->setBaseUrl(settings_->serviceUrl);
    processor_->setMode(settings_->autoCleanEditor ? "auto_correct" : "dictation");
    processor_->setAutoTranslate(settings_->autoTranslate);
    processor_->setProfile("general");
    service_->setContext(settings_->context);
    store_->setDir(settings_->transcriptDirectory());
    loadVadModel();
    hotkeys_->unregisterAll();
    QStringList failed;
    for (const QString &combo : {settings_->insertHotkey, settings_->insertHotkey2, settings_->saveHotkey}) {
        if (!hotkeys_->registerHotkey(combo)) failed << combo;
    }
    return failed.isEmpty() ? QString() : tr("Hotkey unavailable: %1").arg(failed.join(", "));
}

int PanelApp::run() {
    ensureSingleInstance();
    window_ = new MainWindow(this);
    wireHotkeys();
    const QString hotkeyError = applySettings();
    if (!hotkeyError.isEmpty()) QMessageBox::warning(window_, tr("Global hotkeys"), hotkeyError);
    window_->show();

    tray_ = new QSystemTrayIcon(window_);
    tray_->setIcon(window_->windowIcon());
    tray_->setToolTip(tr("Voice panel"));
    auto *menu = new QMenu(window_);
    menu->addAction(tr("Show panel"), window_, &MainWindow::showExpanded);
    menu->addAction(tr("Start dictation"), window_, [this]() { window_->startDictation(false); });
    menu->addAction(tr("Voice playback…"), window_, &MainWindow::openVoicePlayback);
    menu->addAction(tr("Settings..."), window_, &MainWindow::openSettings);
    menu->addAction(tr("Quit"), qApp, &QApplication::quit);
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, window_, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::DoubleClick) window_->showExpanded();
    });
    tray_->show();
    voices_->refresh();
    return exec();
}
