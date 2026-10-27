#include "app/ServiceTray.h"
#include "app/ServerSettingsDialog.h"
#include "app/UiKit.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QPointer>
#include <QProcess>
#include <QUrl>
#include <windows.h>

using namespace VoicePanel;

ServiceTray::ServiceTray(const QString &url, QObject *parent)
    : QObject(parent), menu_(std::make_unique<QMenu>()), url_(url) {
    setObjectName("serviceIndicator");
    client_.setBaseUrl(url);
    // The service is a Python program, not an executable: the script starts it
    // and waits until it is healthy. In a packaged layout the script is absent
    // and the action says so instead of failing silently.
    scriptPath_ = QDir::cleanPath(QDir(QCoreApplication::applicationDirPath())
                                      .absoluteFilePath("../../../tools/start-service.ps1"));
    status_ = menu_->addAction(tr("Voice service · Checking…"));
    status_->setObjectName("serviceStatus");
    status_->setEnabled(false);
    auto *address = menu_->addAction(url);
    address->setEnabled(false);
    menu_->addSeparator();
    auto *open = menu_->addAction(tr("Open panel"));
    connect(open, &QAction::triggered, this, []() {
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    });
    auto *settings = menu_->addAction(tr("Server settings…"));
    settings->setObjectName("serverSettings");
    connect(settings, &QAction::triggered, this, [this]() {
        ServerSettingsDialog dialog(url_);
        dialog.exec();
    });
    start_ = menu_->addAction(tr("Start service"));
    start_->setObjectName("startService");
    start_->setEnabled(false);
    connect(start_, &QAction::triggered, this, &ServiceTray::startService);
    stop_ = menu_->addAction(tr("Stop service"));
    stop_->setObjectName("stopService");
    stop_->setEnabled(false);
    connect(stop_, &QAction::triggered, this, &ServiceTray::stopService);
    menu_->addSeparator();
    auto *quit = menu_->addAction(tr("Close tray icon"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
    icon_.setObjectName("serviceTrayIcon");
    icon_.setContextMenu(menu_.get());
    icon_.setIcon(UiKit::icon("server", "#c9bd9a"));
    icon_.setToolTip(tr("Voice service · Checking…\n%1").arg(url));
    connect(&icon_, &QSystemTrayIcon::activated, this, [open](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick) open->trigger();
    });
    connect(menu_.get(), &QMenu::aboutToShow, this, &ServiceTray::refresh);
    connect(&timer_, &QTimer::timeout, this, &ServiceTray::refresh);
    timer_.start(5000);
    icon_.show();
    refresh();
}

ServiceTray::~ServiceTray() = default;

void ServiceTray::startService() {
    if (starter_) return;
    const QFileInfo script(scriptPath_);
    if (!script.isFile()) {
        icon_.showMessage(tr("Voice service"),
                          tr("Cannot start it from here: %1 is missing.\nRun tools\\start-service.ps1 instead.")
                              .arg(scriptPath_),
                          QSystemTrayIcon::Warning);
        return;
    }
    start_->setEnabled(false);
    status_->setText(tr("Voice service · Starting…"));
    auto *process = new QProcess(this);
    starter_ = process;
    process->setWorkingDirectory(QDir(script.absolutePath()).absoluteFilePath(".."));
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;   // no console flash from the launcher
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || starter_ != process) return;
        starter_ = nullptr;
        icon_.showMessage(tr("Voice service"), process->errorString(), QSystemTrayIcon::Warning);
        process->deleteLater();
        refresh();
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
        if (starter_ != process) return;
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
        starter_ = nullptr;
        process->deleteLater();
        if (code != 0)
            icon_.showMessage(tr("Voice service"),
                              tr("The service did not start: %1").arg(output.right(260)),
                              QSystemTrayIcon::Warning);
        refresh();
    });
    process->start("powershell", {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                  script.absoluteFilePath(), "-Port",
                                  QString::number(QUrl(url_).port(8080))});
}

void ServiceTray::stopService() {
    stop_->setEnabled(false);
    QPointer<ServiceTray> self(this);
    client_.postJson("/admin/shutdown", "{}", [this, self](bool ok, QByteArray, QString error) {
        if (!self) return;
        if (ok) {
            setRunning(false);
            emit stopped();
        } else {
            icon_.showMessage(tr("Voice service"), error, QSystemTrayIcon::Warning);
            refresh();
        }
    });
}

void ServiceTray::refresh() {
    if (checking_ || starter_) return;
    checking_ = true;
    QPointer<ServiceTray> self(this);
    client_.getJson("/health", [this, self](bool ok, QByteArray body, QString) {
        if (!self) return;
        checking_ = false;
        setRunning(ok && QJsonDocument::fromJson(body).object()["status"].toString() == "ok");
    });
}

void ServiceTray::setRunning(bool running) {
    const QString status = running ? tr("Voice service · Running") : tr("Voice service · Offline");
    status_->setText(status);
    // One service action at a time: showing Start next to Stop read as two
    // working buttons and neither explained itself.
    start_->setVisible(!running);
    stop_->setVisible(running);
    start_->setEnabled(!running);
    stop_->setEnabled(running);
    icon_.setToolTip(status + "\n" + url_);
    icon_.setIcon(UiKit::icon("server", running ? "#ee9a3a" : "#e5544e"));
}

int VoicePanel::runServiceTray(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("VoicePanel service indicator");
    app.setQuitOnLastWindowClosed(false);
    UiKit::applyTheme();
    const QString url = app.arguments().value(2, "http://127.0.0.1:8080");
    const QString name = "voicepanel-service-tray-" + QString::number(QUrl(url).port(8080));
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(200)) return 0;
    QLocalServer::removeServer(name);
    QLocalServer single;
    if (!single.listen(name)) return 1;
    QObject::connect(&single, &QLocalServer::newConnection, &single, [&single]() {
        while (auto *socket = single.nextPendingConnection()) socket->deleteLater();
    });
    ServiceTray indicator(url);
    // The icon outlives the service: it is how the service is started again
    // after Stop. Closing it is the "Close tray icon" item in its menu.
    return app.exec();
}
