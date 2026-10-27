#pragma once
#include "core/ServiceClient.h"
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
#include <memory>

class QAction;
class QMenu;
class QProcess;
class PanelTests;

namespace VoicePanel {
class ServiceTray : public QObject {
    Q_OBJECT
public:
    explicit ServiceTray(const QString &url, QObject *parent = nullptr);
    ~ServiceTray() override;

signals:
    void stopped();

private:
    friend class ::PanelTests;
    void refresh();
    void setRunning(bool running);
    void startService();
    void stopService();
    ServiceClient client_;
    QTimer timer_;
    QSystemTrayIcon icon_;
    std::unique_ptr<QMenu> menu_;
    QAction *status_;
    QAction *start_ = nullptr;
    QAction *stop_ = nullptr;
    QProcess *starter_ = nullptr;
    QString scriptPath_;
    QString url_;
    bool checking_ = false;
};

int runServiceTray(int argc, char **argv);
}
