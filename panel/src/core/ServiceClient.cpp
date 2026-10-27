#include "core/ServiceClient.h"

#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonobject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace VoicePanel {

ServiceClient::ServiceClient(QObject *parent) : QObject(parent) { nam_.setTransferTimeout(300000); }

void ServiceClient::setBaseUrl(const QString &url) {
    QString trimmed = url.trimmed();
    while (trimmed.endsWith('/')) trimmed.chop(1);
    if (trimmed != base_) {
        base_ = trimmed;
        emit baseUrlChanged();
    }
}

void ServiceClient::getJson(const QString &path, JsonCallback cb) {
    QNetworkRequest req{QUrl(base_ + path)};
    auto *reply = nam_.get(req);
    connect(reply, &QNetworkReply::finished, [reply, cb]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            cb(true, reply->readAll(), {});
        } else {
            cb(false, reply->readAll(), reply->errorString());
        }
        reply->deleteLater();
    });
}

void ServiceClient::postJson(const QString &path, const QByteArray &json, JsonCallback cb) {
    QNetworkRequest req{QUrl(base_ + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = nam_.post(req, json);
    connect(reply, &QNetworkReply::finished, [reply, cb]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            cb(true, reply->readAll(), {});
        } else {
            cb(false, reply->readAll(), reply->errorString());
        }
        reply->deleteLater();
    });
}

void ServiceClient::deleteJson(const QString &path, JsonCallback cb) {
    QNetworkRequest req{QUrl(base_ + path)};
    auto *reply = nam_.deleteResource(req);
    connect(reply, &QNetworkReply::finished, [reply, cb]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            cb(true, reply->readAll(), {});
        } else {
            cb(false, reply->readAll(), reply->errorString());
        }
        reply->deleteLater();
    });
}

void ServiceClient::patchForm(const QString &path,
                              const QList<QPair<QString, QString>> &fields,
                              JsonCallback cb) {
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const auto &[name, value] : fields) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"%1\"").arg(name));
        part.setBody(value.toUtf8());
        multi->append(part);
    }
    QNetworkRequest req{QUrl(base_ + path)};
    auto *reply = nam_.sendCustomRequest(req, "PATCH", multi);
    multi->setParent(reply);
    connect(reply, &QNetworkReply::finished, [reply, cb]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            cb(true, reply->readAll(), {});
        } else {
            cb(false, reply->readAll(), reply->errorString());
        }
        reply->deleteLater();
    });
}

void ServiceClient::postMultipart(const QString &path,
                                  const QList<QPair<QString, QString>> &fields,
                                  const QString &fileField, const QString &fileName,
                                  const QByteArray &fileBytes, const QString &contentType,
                                  JsonCallback cb) {
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const auto &[name, value] : fields) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"%1\"").arg(name));
        part.setBody(value.toUtf8());
        multi->append(part);
    }
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"%1\"; filename=\"%2\"").arg(fileField, fileName));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    filePart.setBody(fileBytes);
    multi->append(filePart);

    QNetworkRequest req{QUrl(base_ + path)};
    auto *reply = nam_.post(req, multi);
    multi->setParent(reply);
    connect(reply, &QNetworkReply::finished, [reply, cb]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            cb(true, reply->readAll(), {});
        } else {
            cb(false, reply->readAll(), reply->errorString());
        }
        reply->deleteLater();
    });
}

void ServiceClient::transcribe(const QByteArray &wav, const QString &model,
                               const QString &language,
                               std::function<void(bool, QString, QString)> cb) {
    QList<QPair<QString, QString>> fields{{"response_format", "json"}};
    if (!context_.isEmpty()) fields.append({"prompt", context_});
    if (model != "default") fields.append({"model", model});
    if (!language.isEmpty() && language != "auto") fields.append({"language", language});
    postMultipart("/v1/audio/transcriptions", fields, "file", "dictation.wav", wav,
                  "audio/wav", [cb](bool ok, QByteArray body, QString error) mutable {
                      if (!ok) {
                          // body may carry a JSON error detail
                          QJsonParseError pe;
                          auto doc = QJsonDocument::fromJson(body, &pe);
                          if (pe.error == QJsonParseError::NoError) {
                              auto detail = doc.object().value("detail").toString();
                              if (!detail.isEmpty()) error = detail;
                          }
                          cb(false, {}, error);
                          return;
                      }
                      auto doc = QJsonDocument::fromJson(body);
                      cb(true, doc.object().value("text").toString(), {});
                  });
}

void ServiceClient::processText(const QString &text, const QString &mode,
                                const QString &profile, const QString &previous,
                                const QList<SpellingHint> &spelling,
                                std::function<void(bool, QString, QString, QString)> cb) {
    QJsonObject payload{{"text", text}, {"mode", mode}, {"profile", profile}};
    if (!context_.isEmpty()) payload.insert("context", context_);
    if (!previous.isEmpty()) payload.insert("previous", previous);
    if (!spelling.isEmpty()) {
        QJsonArray hints;
        for (const auto &hint : spelling) {
            QJsonArray suggestions;
            for (const auto &suggestion : hint.suggestions) suggestions.append(suggestion);
            hints.append(QJsonObject{{"word", hint.word}, {"suggestions", suggestions}});
        }
        payload.insert("spelling", hints);
    }
    postJson("/v1/text/process", QJsonDocument(payload).toJson(), [cb](bool ok, QByteArray body, QString err) mutable {
        if (!ok) {
            QJsonParseError pe;
            auto doc = QJsonDocument::fromJson(body, &pe);
            if (pe.error == QJsonParseError::NoError) {
                auto detail = doc.object().value("detail").toString();
                if (!detail.isEmpty()) err = detail;
            }
            cb(false, {}, {}, err);
            return;
        }
        auto obj = QJsonDocument::fromJson(body).object();
        cb(true, obj.value("result").toString(), obj.value("notes").toString(), {});
    });
}

void ServiceClient::speech(const QString &text, const QString &model, const QString &voice,
                           double speed,
                           std::function<void(bool, QByteArray, QString)> cb, const QString &language) {
    QJsonObject payload{{"input", text}, {"response_format", "wav"}};
    if (!model.isEmpty()) payload.insert("model", model);
    if (!voice.isEmpty()) payload.insert("voice", voice);
    if (!language.isEmpty()) payload.insert("language", language);
    payload.insert("speed", speed);
    QNetworkRequest req{QUrl(base_ + "/v1/audio/speech")};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = nam_.post(req, QJsonDocument(payload).toJson());
    connect(reply, &QNetworkReply::finished, [reply, cb]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            cb(true, reply->readAll(), {});
        } else {
            const auto detail = QJsonDocument::fromJson(reply->readAll()).object().value("detail").toString();
            cb(false, {}, detail.isEmpty() ? reply->errorString() : detail);
        }
        reply->deleteLater();
    });
}

} // namespace VoicePanel
