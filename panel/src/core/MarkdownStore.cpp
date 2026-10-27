#include "core/MarkdownStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>

using namespace VoicePanel;

MarkdownStore::MarkdownStore(QObject *parent) : QObject(parent) {}

void MarkdownStore::setDir(const QString &dir) {
    dir_ = dir;
    if (!dir_.isEmpty()) {
        QDir().mkpath(dir_);
    }
}

static QString yamlEscape(const QString &s) {
    QString out = s;
    out.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n");
    return "\"" + out + "\"";
}

bool MarkdownStore::saveRecord(const SessionRecord &record, QString *errorOut) {
    if (!enabled()) return false;
    const QString dateDir = QDir(dir_).filePath(record.started.toString("yyyy-MM-dd"));
    if (!QDir().mkpath(dateDir)) {
        if (errorOut) *errorOut = tr("cannot create %1").arg(dateDir);
        return false;
    }
    const QString path = QDir(dateDir).filePath(
        QString("session-%1-%2.md").arg(record.started.toString("HHmmss"), record.sessionId));

    QStringList lines;
    lines << "---";
    lines << "session_id: " + yamlEscape(record.sessionId);
    lines << "started: " + record.started.toString(Qt::ISODate);
    lines << "ended: " + record.ended.toString(Qt::ISODate);
    lines << "mode: " + yamlEscape(record.mode);
    if (!record.sourceLang.isEmpty()) lines << "source_language: " + yamlEscape(record.sourceLang);
    if (!record.targetLang.isEmpty()) lines << "target_language: " + yamlEscape(record.targetLang);
    lines << "---";
    lines << "";
    if (!record.original.isEmpty()) {
        lines << "## Original transcription";
        lines << "";
        lines << record.original;
        lines << "";
    }
    if (!record.corrected.isEmpty() && record.corrected != record.original) {
        lines << "## Corrected";
        lines << "";
        lines << record.corrected;
        lines << "";
    }
    if (!record.translation.isEmpty()) {
        lines << "## Translation";
        lines << "";
        lines << record.translation;
        lines << "";
    }
    if (!record.finalText.isEmpty()) {
        lines << "## Final (user-confirmed)";
        lines << "";
        lines << record.finalText;
        lines << "";
    }

    QSaveFile file(path);   // atomic: renamed on commit
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = tr("cannot open %1").arg(path);
        return false;
    }
    QTextStream ts(&file);
    ts.setEncoding(QStringConverter::Utf8);
    ts << lines.join("\n") << "\n";
    if (!file.commit()) {
        if (errorOut) *errorOut = tr("write failed for %1").arg(path);
        return false;
    }
    return true;
}

void MarkdownStore::enforceAudioRetention(const QString &audioDir, int days) {
    if (audioDir.isEmpty() || days <= 0) return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-qint64(days));
    const QDir dir(audioDir);
    for (const QFileInfo &f : dir.entryInfoList({"*.wav"}, QDir::Files)) {
        if (f.lastModified() < cutoff) QFile::remove(f.absoluteFilePath());
    }
}
