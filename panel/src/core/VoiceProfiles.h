#pragma once
#include "core/ServiceClient.h"
#include <QObject>
#include <QStringList>

// Voice profile (zero-shot cloning) access through the service API.
namespace VoicePanel {

struct VoiceProfile {
    QString id;
    QString name;
    QString model;
    QString language;
    QString referenceText;
    double referenceDurationS = 0.0;
};

class VoiceProfiles : public QObject {
    Q_OBJECT
public:
    explicit VoiceProfiles(ServiceClient *service, QObject *parent = nullptr);

    void refresh();
    QList<VoiceProfile> profiles() const { return profiles_; }
    void create(const QString &name, const QString &wavPath, const QString &referenceText,
                const QString &language);
    void update(const QString &id, const QString &name, const QString &referenceText,
                const QString &language);
    void remove(const QString &id);
    void preview(const QString &id);

signals:
    void listChanged();
    void created(bool ok, const QString &detail);
    void updated(bool ok, const QString &detail);
    void removed(bool ok);
    void previewReady(bool ok, const QString &filePathOrError);

private:
    ServiceClient *service_;
    QList<VoiceProfile> profiles_;
};

} // namespace VoicePanel
