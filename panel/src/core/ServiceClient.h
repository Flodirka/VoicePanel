#pragma once
#include "core/SpellingChecker.h"
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <functional>

// Thin async HTTP client for the Voice AI Service. All callbacks run on the
// GUI thread via queued signals; errors surface as (false, message).
namespace VoicePanel {

class ServiceClient : public QObject {
    Q_OBJECT
public:
    using BytesCallback = std::function<void(bool ok, QByteArray body, QString error)>;
    using JsonCallback = std::function<void(bool ok, QByteArray body, QString error)>;

    explicit ServiceClient(QObject *parent = nullptr);
    void setBaseUrl(const QString &url);
    void setContext(const QString &context) { context_ = context; }

    void getJson(const QString &path, JsonCallback cb);
    void postJson(const QString &path, const QByteArray &json, JsonCallback cb);
    void deleteJson(const QString &path, JsonCallback cb);
    void patchForm(const QString &path, const QList<QPair<QString, QString>> &fields,
                   JsonCallback cb);
    void postMultipart(const QString &path,
                       const QList<QPair<QString, QString>> &fields,
                       const QString &fileField, const QString &fileName,
                       const QByteArray &fileBytes, const QString &contentType,
                       JsonCallback cb);

    // convenience: transcribe WAV bytes -> text
    void transcribe(const QByteArray &wav, const QString &model, const QString &language,
                    std::function<void(bool ok, QString text, QString error)> cb);
    // text processing (mode/profile from Settings); spelling hints come from the
    // Windows spell checker and only inform the model, nothing is replaced here
    void processText(const QString &text, const QString &mode, const QString &profile,
                     const QString &previous, const QList<SpellingHint> &spelling,
                     std::function<void(bool ok, QString result, QString notes, QString error)> cb);
    // TTS -> WAV bytes
    void speech(const QString &text, const QString &model, const QString &voice, double speed,
                std::function<void(bool ok, QByteArray wav, QString error)> cb,
                const QString &language = {});

signals:
    void baseUrlChanged();

private:
    QNetworkAccessManager nam_;
    QString base_;
    QString context_;
};

} // namespace VoicePanel
