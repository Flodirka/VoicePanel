#pragma once
#include <QDialog>
#include <QMap>
#include <QStringList>

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class PanelTests;

namespace VoicePanel {
class ServiceClient;

// CPU/GPU placement for the three inference processes of the service:
// translation/correction, cloned-voice TTS, and the shared recognition and
// built-in voice process. The window reads the current choice from the service
// and writes nothing until Save; the service applies a change only while the
// affected process is idle.
class ServerSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServerSettingsDialog(const QString &serviceUrl, QWidget *parent = nullptr);

private:
    friend class ::PanelTests;
    struct Row {
        QComboBox *device = nullptr;
        QLabel *state = nullptr;
    };

    void buildUi();
    void load();
    void save();
    void showSnapshot(const QByteArray &body);
    void restoreChoices();
    void updateSaveState();
    QString errorDetail(const QByteArray &body, const QString &fallback) const;

    ServiceClient *client_ = nullptr;
    QVBoxLayout *rowsLayout_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QMap<QString, Row> rows_;
    QMap<QString, QString> serverDevices_;
    bool loading_ = false;
};
} // namespace VoicePanel
