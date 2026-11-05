#include "core/Settings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace VoicePanel {

QString Settings::transcriptDirectory() const {
    if (!markdownDir.isEmpty() || !autoSaveSessions) return markdownDir;
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/transcripts";
}

void Settings::load() {
    QSettings s;
    s.beginGroup("panel");
    serviceUrl = s.value("serviceUrl", serviceUrl).toString();
    mode = "auto_correct";
    profile = "general";
    contextDocument = s.value("contextDocument", QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation) + "/context.md").toString();
    loadContext();
    asrModel = s.value("asrModel", asrModel).toString();
    meetingModel = QStringLiteral("nemotron-diar");
    meetingDiarize = s.value("meetingDiarize", meetingDiarize).toBool();
    meetingLanguage = s.value("meetingLanguage", meetingLanguage).toInt();
    liveOutgoing = s.value("liveOutgoing", liveOutgoing).toInt();
    callLanguage = s.value("callLanguage", callLanguage).toInt();
    speakCaptions = s.value("speakCaptions", speakCaptions).toBool();
    subtitlesLanguage = s.value("subtitlesLanguage", subtitlesLanguage).toInt();
    language = "auto";
    inputDevice = s.value("inputDevice", inputDevice).toString();
    systemAudioDevice = s.value("systemAudioDevice", systemAudioDevice).toString();
    outputDevice = s.value("outputDevice", outputDevice).toInt();
    vadModel = s.value("vadModel", vadModel).toString();

    speechThreshold = s.value("speechThreshold", speechThreshold).toDouble();
    silenceFrames = s.value("silenceFrames", silenceFrames).toInt();
    minSpeechFrames = s.value("minSpeechFrames", minSpeechFrames).toInt();
    prerollFrames = s.value("prerollFrames", prerollFrames).toInt();
    maxUtteranceS = s.value("maxUtteranceS", maxUtteranceS).toInt();
    speculateEveryFrames = s.value("speculateEveryFrames", speculateEveryFrames).toInt();
    speculative = s.value("speculative", speculative).toBool();

    quickInsert = s.value("quickInsert", quickInsert).toBool();
    reviewBeforeInsert = s.value("reviewBeforeInsert", reviewBeforeInsert).toBool();
    autoCleanEditor = s.value("autoCleanEditor", autoCleanEditor).toBool();
    transcriptResetS = s.value("transcriptResetS", transcriptResetS).toInt();
    autoTranslate = s.value("autoTranslate", autoTranslate).toBool();
    insertHotkey = s.value("insertHotkey", insertHotkey).toString();
    insertHotkey2 = s.value("insertHotkey2", insertHotkey2).toString();
    saveHotkey = s.value("saveHotkey", saveHotkey).toString();

    ttsModel = s.value("ttsModel", ttsModel).toString();
    ttsVoiceProfile = s.value("ttsVoiceProfile", ttsVoiceProfile).toString();
    ttsRussianVoice = s.value("ttsRussianVoice", ttsVoiceProfile).toString();
    ttsEnglishVoice = s.value("ttsEnglishVoice", ttsVoiceProfile).toString();
    // An empty entry is a preference saved before a voice was recorded, not a
    // choice of the built-in voice ("builtin" is that choice): use the profile.
    if (ttsRussianVoice.isEmpty()) ttsRussianVoice = ttsVoiceProfile;
    if (ttsEnglishVoice.isEmpty()) ttsEnglishVoice = ttsVoiceProfile;
    ttsSpeed = s.value("ttsSpeed", ttsSpeed).toDouble();
    ttsPauseMs = s.value("ttsPauseMs", ttsPauseMs).toInt();
    ttsEnabled = s.value("ttsEnabled", ttsEnabled).toBool();

    markdownDir = s.value("markdownDir", markdownDir).toString();
    autoSaveSessions = s.value("autoSaveSessions", autoSaveSessions).toBool();
    audioRetentionDays = s.value("audioRetentionDays", audioRetentionDays).toInt();

    compactGeometry = s.value("compactGeometry").toByteArray();
    editorGeometry = s.value("editorGeometry").toByteArray();
    dockPosition = s.value("dockPosition", dockPosition).toString();
    s.endGroup();
}

void Settings::save() const {
    QSettings s;
    s.beginGroup("panel");
    s.setValue("serviceUrl", serviceUrl);
    s.setValue("mode", mode);
    s.setValue("profile", profile);
    s.setValue("contextDocument", contextDocument);
    s.setValue("asrModel", asrModel);
    s.setValue("meetingModel", meetingModel);
    s.setValue("meetingDiarize", meetingDiarize);
    s.setValue("meetingLanguage", meetingLanguage);
    s.setValue("liveOutgoing", liveOutgoing);
    s.setValue("callLanguage", callLanguage);
    s.setValue("speakCaptions", speakCaptions);
    s.setValue("subtitlesLanguage", subtitlesLanguage);
    s.setValue("language", language);
    s.setValue("inputDevice", inputDevice);
    s.setValue("systemAudioDevice", systemAudioDevice);
    s.setValue("outputDevice", outputDevice);
    s.setValue("vadModel", vadModel);

    s.setValue("speechThreshold", speechThreshold);
    s.setValue("silenceFrames", silenceFrames);
    s.setValue("minSpeechFrames", minSpeechFrames);
    s.setValue("prerollFrames", prerollFrames);
    s.setValue("maxUtteranceS", maxUtteranceS);
    s.setValue("speculateEveryFrames", speculateEveryFrames);
    s.setValue("speculative", speculative);

    s.setValue("quickInsert", quickInsert);
    s.setValue("reviewBeforeInsert", reviewBeforeInsert);
    s.setValue("autoCleanEditor", autoCleanEditor);
    s.setValue("transcriptResetS", transcriptResetS);
    s.setValue("autoTranslate", autoTranslate);
    s.setValue("insertHotkey", insertHotkey);
    s.setValue("insertHotkey2", insertHotkey2);
    s.setValue("saveHotkey", saveHotkey);

    s.setValue("ttsModel", ttsModel);
    s.setValue("ttsVoiceProfile", ttsVoiceProfile);
    s.setValue("ttsRussianVoice", ttsRussianVoice);
    s.setValue("ttsEnglishVoice", ttsEnglishVoice);
    s.setValue("ttsSpeed", ttsSpeed);
    s.setValue("ttsPauseMs", ttsPauseMs);
    s.setValue("ttsEnabled", ttsEnabled);

    s.setValue("markdownDir", markdownDir);
    s.setValue("autoSaveSessions", autoSaveSessions);
    s.setValue("audioRetentionDays", audioRetentionDays);

    s.setValue("compactGeometry", compactGeometry);
    s.setValue("editorGeometry", editorGeometry);
    s.setValue("dockPosition", dockPosition);
    s.endGroup();
}

void Settings::loadContext() {
    QFile file(contextDocument);
    if (file.open(QIODevice::ReadOnly)) context = QString::fromUtf8(file.readAll());
    else {
        QFile defaults(":/default-context.md");
        if (defaults.open(QIODevice::ReadOnly)) context = QString::fromUtf8(defaults.readAll());
    }
}

bool Settings::saveContext(QString *errorOut) const {
    QDir().mkpath(QFileInfo(contextDocument).absolutePath());
    QSaveFile file(contextDocument);
    const QByteArray bytes = context.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (errorOut) *errorOut = file.errorString();
        return false;
    }
    return true;
}

QStringList Settings::termsFromContext(const QString &context) {
    // A heading whose text names the section in any of the supported languages:
    //   ## Terminology   /   ## Терминология   /   # Glossary
    // Inside the section only bullet lines ("- term", "• term", "* term") are
    // terms, one per line; plain lines are comments. The section ends at the
    // next heading. Outside it the free text is only prompt context.
    static const QRegularExpression heading(
        QStringLiteral("^\\s*#+\\s*.*(terminolog|терминолог|glossary|словарь)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression anyHeading(QStringLiteral("^\\s*#"));
    static const QRegularExpression bullet(QStringLiteral("^\\s*[-*•]\\s+"));
    QStringList terms;
    bool inSection = false;
    for (const QString &raw : context.split(QLatin1Char('\n'))) {
        if (anyHeading.match(raw).hasMatch()) {
            inSection = heading.match(raw).hasMatch();
            continue;
        }
        if (!inSection) continue;
        const QRegularExpressionMatch bulletHit = bullet.match(raw);
        if (!bulletHit.hasMatch()) continue;
        const QString line = raw.mid(bulletHit.capturedLength()).trimmed();
        if (!line.isEmpty() && !terms.contains(line, Qt::CaseInsensitive)) terms << line;
    }
    return terms;
}

} // namespace VoicePanel
