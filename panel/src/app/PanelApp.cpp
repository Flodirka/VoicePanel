#include "app/PanelApp.h"

#include "app/MainWindow.h"
#include "app/CaptionOverlay.h"
#include "app/MeetingWindow.h"
#include "app/SessionWindow.h"
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
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <windows.h>

using namespace VoicePanel;

namespace {
const QString kInterpreterHotkey = QStringLiteral("Ctrl+Alt+T");
const QString kMeetingHotkey = QStringLiteral("Ctrl+Alt+M");
}

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
    service_->setDictionary(Settings::termsFromContext(settings_->context));
    if (!QFile::exists(settings_->contextDocument)) settings_->saveContext();
    dictation_ = std::make_unique<DictationController>(service_.get());
    processor_ = std::make_unique<TextProcessor>(service_.get());
    processor_->setDictionary(Settings::termsFromContext(settings_->context));
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
            vadPath_ = c;
            break;
        }
    }
}

PanelApp::~PanelApp() {
    delete overlay_;
    delete meetingWindow_;
    delete sessionWindow_;
    delete window_;
}

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

void PanelApp::openSession(int mode) {
    if (mode == 1) {
        if (!meetingWindow_) meetingWindow_ = new MeetingWindow(this);
        meetingWindow_->show();
        meetingWindow_->raise();
        meetingWindow_->activateWindow();
        return;
    }
    if (!sessionWindow_) sessionWindow_ = new SessionWindow(this);
    sessionWindow_->openMode(SessionWindow::Mode::Interpreter);
}

void PanelApp::openSubtitles() {
    if (!overlay_) overlay_ = new CaptionOverlay(this);
    overlay_->begin();
}

void PanelApp::wireHotkeys() {
    connect(hotkeys_.get(), &HotkeyManager::triggered, window_, [this](const QString &combo) {
        if (combo == settings_->insertHotkey) {
            window_->toggleDictation(false);
        } else if (combo == settings_->insertHotkey2) {
            window_->toggleDictation(true);
        } else if (combo == settings_->saveHotkey) {
            window_->saveReviewedText();
        } else if (combo == kInterpreterHotkey) {
            openSession(0);
        } else if (combo == kMeetingHotkey) {
            openSession(1);
        }
    });
}

QString PanelApp::applySettings() {
    service_->setBaseUrl(settings_->serviceUrl);
    processor_->setMode(settings_->autoCleanEditor ? "auto_correct" : "dictation");
    processor_->setAutoTranslate(settings_->autoTranslate);
    processor_->setProfile("general");
    service_->setContext(settings_->context);
    service_->setDictionary(Settings::termsFromContext(settings_->context));
    processor_->setDictionary(Settings::termsFromContext(settings_->context));
    store_->setDir(settings_->transcriptDirectory());
    loadVadModel();
    hotkeys_->unregisterAll();
    QStringList failed;
    for (const QString &combo : {settings_->insertHotkey, settings_->insertHotkey2,
                                 settings_->saveHotkey}) {
        if (!hotkeys_->registerHotkey(combo)) failed << combo;
    }
    // The live modes are also in the tray menu, so a hotkey that another
    // application already owns must not produce a warning on every start.
    for (const QString &combo : {kInterpreterHotkey, kMeetingHotkey}) {
        hotkeys_->registerHotkey(combo);
    }
    return failed.isEmpty() ? QString() : tr("Hotkey unavailable: %1").arg(failed.join(", "));
}

int PanelApp::runCheckBundle() {
    // No window, tray, single-instance lock or message loop, and the
    // microphone is never opened: a packaged build must prove its parts
    // resolve before the user starts it. Prints one PASS/FAIL line per check.
    // The report lands in check-bundle.txt next to the exe: the exe is a
    // WIN32 app, so console output is unreliable when launched redirected,
    // and AttachConsole/freopen on CONOUT$ can crash in that case.
    QFile report(QCoreApplication::applicationDirPath() + "/check-bundle.txt");
    const bool reportOpen = report.open(QIODevice::WriteOnly | QIODevice::Text);
    int failures = 0;
    const auto check = [&failures, &report, reportOpen](const QString &name, bool ok, const QString &detail = {}) {
        const QString line = QStringLiteral("%1 %2 %3\n").arg(ok ? "PASS" : "FAIL", name.leftJustified(28), detail);
        if (reportOpen) report.write(line.toUtf8());
        qDebug().noquote() << line.trimmed();
        if (!ok) ++failures;
    };
    check("settings", true, settings_->contextDocument);
    check("context document", QFile::exists(settings_->contextDocument), settings_->contextDocument);
    const QStringList terms = Settings::termsFromContext(settings_->context);
    check("terminology dictionary", true, QStringLiteral("%1 term(s)").arg(terms.size()));
    check("VAD model", !vadPath_.isEmpty() && QFile::exists(vadPath_), vadPath_);
    const QString hotkeyError = applySettings();   // registers the real hotkeys
    check("hotkeys", hotkeyError.isEmpty(), hotkeyError);
    hotkeys_->unregisterAll();
    // Service reachability: one synchronous /health probe with a short timeout.
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(settings_->serviceUrl + "/health")));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    loop.exec();
    bool serviceOk = !timedOut && reply->error() == QNetworkReply::NoError;
    QString detail = timedOut ? QStringLiteral("no answer within 3 s") : reply->errorString();
    if (serviceOk) detail = QString::fromUtf8(reply->readAll()).left(120);
    reply->deleteLater();
    check("service /health", serviceOk, detail);
    const QString summary = QStringLiteral("bundle check: %1 (%2 failure(s))\n")
        .arg(failures ? "FAILED" : "OK").arg(failures);
    if (reportOpen) { report.write(summary.toUtf8()); report.close(); }
    qDebug().noquote() << summary.trimmed();
    return failures ? 1 : 0;
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
    menu->addAction(tr("Live interpreter…"), window_, [this]() { openSession(0); });
    menu->addAction(tr("Meeting transcript…"), window_, [this]() { openSession(1); });
    menu->addAction(tr("Subtitles overlay…"), window_, [this]() { openSubtitles(); });
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
