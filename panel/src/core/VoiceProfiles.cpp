#include "core/VoiceProfiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

using namespace VoicePanel;

VoiceProfiles::VoiceProfiles(ServiceClient *service, QObject *parent)
    : QObject(parent), service_(service) {}

void VoiceProfiles::refresh() {
    service_->getJson("/v1/voices", [this](bool ok, QByteArray body, QString) {
        profiles_.clear();
        if (!ok) {
            emit listChanged();
            return;
        }
        const auto arr = QJsonDocument::fromJson(body).object().value("voices").toArray();
        for (const auto &v : arr) {
            const auto o = v.toObject();
            VoiceProfile p;
            p.id = o.value("id").toString();
            p.name = o.value("name").toString();
            p.model = o.value("model").toString();
            p.language = o.value("language").toString();
            p.referenceText = o.value("reference_text").toString();
            p.referenceDurationS = o.value("reference_duration_s").toDouble();
            profiles_.append(p);
        }
        emit listChanged();
    });
}

void VoiceProfiles::create(const QString &name, const QString &wavPath,
                           const QString &referenceText, const QString &language) {
    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit created(false, tr("cannot read %1").arg(wavPath));
        return;
    }
    const QByteArray audio = f.readAll();
    f.close();
    service_->postMultipart(
        "/v1/voices",
        {{"name", name},
         {"reference_text", referenceText},
         {"language", language},
         {"transcribe", referenceText.isEmpty() ? "true" : "false"}},
        "file", QFileInfo(wavPath).fileName(), audio, "audio/wav",
        [this](bool ok, QByteArray body, QString err) {
            QString detail = err;
            if (ok) {
                detail = QJsonDocument::fromJson(body).object().value("voice").toObject().value("id").toString();
                refresh();
            } else {
                const auto obj = QJsonDocument::fromJson(body).object();
                if (!obj.value("detail").toString().isEmpty()) detail = obj.value("detail").toString();
            }
            emit created(ok, detail);
        });
}

void VoiceProfiles::update(const QString &id, const QString &name, const QString &referenceText,
                           const QString &language) {
    service_->patchForm(
        "/v1/voices/" + id,
        {{"name", name}, {"reference_text", referenceText}, {"language", language}},
        [this](bool ok, QByteArray body, QString err) {
            QString detail = err;
            if (!ok) {
                const auto obj = QJsonDocument::fromJson(body).object();
                if (!obj.value("detail").toString().isEmpty()) detail = obj.value("detail").toString();
            }
            if (ok) refresh();
            emit updated(ok, detail);
        });
}

void VoiceProfiles::remove(const QString &id) {
    service_->deleteJson("/v1/voices/" + id, [this](bool ok, QByteArray, QString) {
        if (ok) refresh();
        emit removed(ok);
    });
}

void VoiceProfiles::preview(const QString &id) {
    service_->postJson("/v1/voices/" + id + "/preview", "{}",
                       [this](bool ok, QByteArray body, QString err) {
        if (!ok) {
            QString detail = err;
            const auto obj = QJsonDocument::fromJson(body).object();
            if (!obj.value("detail").toString().isEmpty()) detail = obj.value("detail").toString();
            emit previewReady(false, detail);
            return;
        }
        const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                               + "/voicepanel";
        QDir().mkpath(tmpDir);
        const QString path = tmpDir + "/preview-" +
                             QString::number(QDateTime::currentMSecsSinceEpoch()) + ".wav";
        QFile out(path);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(body);
            out.close();
            emit previewReady(true, path);
        } else {
            emit previewReady(false, tr("cannot write preview file"));
        }
    });
}
