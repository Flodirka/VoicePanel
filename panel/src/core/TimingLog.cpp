#include "core/TimingLog.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

namespace VoicePanel::TimingLog {

namespace {
QElapsedTimer &clock() {
    static QElapsedTimer timer = [] { QElapsedTimer t; t.start(); return t; }();
    return timer;
}
const QString &path() {
    static const QString value = qEnvironmentVariable("VOICEPANEL_TIMING_LOG");
    return value;
}
} // namespace

bool enabled() { return !path().isEmpty(); }

qint64 nowMs() { return clock().elapsed(); }

void event(const char *name, const QJsonObject &fields) {
    if (!enabled()) return;
    QJsonObject line = fields;
    line.insert("event", QString::fromLatin1(name));
    line.insert("t_ms", double(nowMs()));
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    file.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
}

} // namespace VoicePanel::TimingLog
