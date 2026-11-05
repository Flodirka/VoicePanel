#pragma once
#include "core/SpellingChecker.h"
#include <QByteArray>
#include <QJsonArray>
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
    // User terminology (the context document's Terminology section). Sent with
    // every text request and warm-up so the service can fix near-misses of
    // these words deterministically before the model runs.
    void setDictionary(const QStringList &terms) { dictionary_ = terms; }

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
    // Speaker turns for one utterance: [{"start_sample","end_sample","speaker_id"}]
    void diarize(const QByteArray &wav,
                 std::function<void(bool ok, QJsonArray turns, QString error)> cb);
    void transcribe(const QByteArray &wav, const QString &model, const QString &language,
                    std::function<void(bool ok, QString text, QString error)> cb);
    // text processing (mode/profile from Settings); spelling hints come from the
    // Windows spell checker and only inform the model, nothing is replaced here
    void processText(const QString &text, const QString &mode, const QString &profile,
                     const QString &previous, const QList<SpellingHint> &spelling,
                     std::function<void(bool ok, QString result, QString notes, QString error)> cb);
    // Fire-and-forget: ask the service to load the recognition model and
    // prefill the text model's prompt while the user starts speaking. A service
    // without /v1/warmup answers 404, which is ignored.
    void warmup(const QString &mode, const QString &profile);
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
    QStringList dictionary_;
};

} // namespace VoicePanel
