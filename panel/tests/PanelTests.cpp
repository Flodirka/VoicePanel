#include "app/MainWindow.h"
#include "app/MeetingWindow.h"
#include "app/SessionWindow.h"
#include "app/CaptionOverlay.h"
#include "app/PanelApp.h"
#include "app/ServerSettingsDialog.h"
#include "app/SettingsDialog.h"
#include "app/VoicePlaybackDialog.h"
#include "app/VoiceDesignWidget.h"
#include "app/ServiceTray.h"
#include "core/AudioMeter.h"
#include "core/DictationController.h"
#include "core/LevelBadge.h"
#include "core/MarkdownStore.h"
#include "core/ServiceClient.h"
#include "core/SessionController.h"
#include "core/Settings.h"
#include "core/TextProcessor.h"
#include "core/TtsPlayer.h"
#include "core/HotkeyManager.h"
#include "audio/WasapiCapture.h"
#include <QElapsedTimer>
#include <QScreen>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QtEndian>
#include <QPainter>
#include <QPointer>
#include <QLineEdit>
#include "core/TextInserter.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QTextBlock>
#include <QDirIterator>
#include <QDataStream>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSpinBox>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QtTest>
#include <windows.h>

using namespace VoicePanel;

class TestServer : public QObject {
public:
    QTcpServer server;
    QList<QPointer<QTcpSocket>> requests;
    QStringList targets;               // request line per entry, e.g. "POST /v1/audio/diarize HTTP/1.1"
    explicit TestServer() {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes]() {
                    *bytes += socket->readAll();
                    const int headerEnd = bytes->indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("received").toBool()) return;
                    const auto headers = bytes->left(headerEnd).split('\n');
                    int length = 0;
                    for (const auto &header : headers)
                        if (header.toLower().startsWith("content-length:"))
                            length = header.mid(15).trimmed().toInt();
                    if (bytes->size() < headerEnd + 4 + length) return;
                    socket->setProperty("received", true);
                    socket->setProperty("body", bytes->mid(headerEnd + 4, length));
                    if (headers.first().startsWith("GET /health ")) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"status\":\"ok\"}");
                        socket->disconnectFromHost();
                        return;
                    }
                    requests << socket;
                    targets << QString::fromLatin1(headers.first()).trimmed();
                });
            }
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1").arg(server.serverPort()); }
    void reply(int index, const QByteArray &body, bool ok = true) {
        auto *socket = requests[index].data();
        if (!socket) return;
        socket->write(QByteArray(ok ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 502 Bad Gateway\r\n")
                      + "Content-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

class PanelTests : public QObject {
    Q_OBJECT
private:
    PanelApp *app_ = static_cast<PanelApp *>(qApp);

    static QPushButton *button(QWidget &widget, const QString &text) {
        for (auto *b : widget.findChildren<QPushButton *>())
            if (b->text().contains(text)) return b;
        return nullptr;
    }

    // 16 kHz mono PCM16, the only format the live sessions produce.
    static QVector<float> readWav16k(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        const QByteArray all = file.readAll();
        if (all.size() < 44 || !all.startsWith("RIFF")) return {};
        int offset = 12, dataOffset = -1, dataSize = 0;
        while (offset + 8 <= all.size()) {
            const quint32 size = qFromLittleEndian<quint32>(all.constData() + offset + 4);
            if (all.mid(offset, 4) == "data") {
                dataOffset = offset + 8;
                dataSize = int(qMin<quint32>(size, quint32(all.size() - dataOffset)));
                break;
            }
            offset += 8 + int(size) + (int(size) & 1);
        }
        if (dataOffset < 0) return {};
        QVector<float> samples(dataSize / 2);
        for (int i = 0; i < samples.size(); ++i)
            samples[i] = qFromLittleEndian<qint16>(all.constData() + dataOffset + 2 * i) / 32768.0f;
        return samples;
    }

    static int requestIndex(const TestServer &server, const QString &needle, int from = 0) {
        for (int i = from; i < server.requests.size(); ++i)
            if (server.targets.value(i).contains(needle)) return i;
        return -1;
    }

    // One phrase per stretch of speech, closing after ~0.6 s of silence the way
    // the live session's VAD does, so a long fixture can drive a controller
    // phrase by phrase instead of in real time.
    static QList<QPair<int, int>> splitPhrases(const QVector<float> &call) {
        QList<QPair<int, int>> phrases;
        constexpr int frame = 800;              // 50 ms at 16 kHz
        int speechStart = -1, silence = 0;
        for (int offset = 0; offset < call.size(); offset += frame) {
            double energy = 0;
            for (int i = offset; i < offset + frame && i < call.size(); ++i) energy += call[i] * call[i];
            const bool speech = qSqrt(energy / frame) > 0.01;
            if (speech) {
                if (speechStart < 0) speechStart = offset;
                silence = 0;
            } else if (speechStart >= 0 && (silence += frame) >= 9600) {
                if (offset + frame - speechStart >= 16000)
                    phrases.append(qMakePair(speechStart, offset + frame - speechStart));
                speechStart = -1;
                silence = 0;
            }
            if (speechStart >= 0 && offset + frame - speechStart >= 16000 * 12) {   // hard cap like the VAD
                phrases.append(qMakePair(speechStart, 16000 * 12));
                speechStart = -1;
                silence = 0;
            }
        }
        return phrases;
    }

    // 16 kHz mono PCM16 bytes for a helper that has to post audio itself.
    static QByteArray wav16k(const QVector<float> &samples) {
        QByteArray wav;
        QDataStream out(&wav, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::LittleEndian);
        const quint32 dataSize = quint32(samples.size()) * 2;
        out.writeRawData("RIFF", 4);
        out << quint32(36 + dataSize);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1) << quint32(16000) << quint32(32000)
            << quint16(2) << quint16(16);
        out.writeRawData("data", 4);
        out << dataSize;
        for (float value : samples) out << qint16(qBound(-1.0f, value, 1.0f) * 32767.0f);
        return wav;
    }

    // One diarisation of a whole recording, used as the acceptance reference:
    // the engine hears every voice at once, so this is the separation the live
    // rolling window must reproduce phrase by phrase.
    static QJsonArray diarizeWhole(ServiceClient *service, const QVector<float> &call) {
        QJsonArray result;
        bool done = false;
        service->diarize(wav16k(call),
                         [&result, &done](bool ok, QJsonArray turns, QString) {
                             if (ok) result = turns;
                             done = true;
                         });
        QElapsedTimer clock;
        clock.start();
        while (!done && clock.elapsed() < 180000) QTest::qWait(50);
        return result;
    }

    // ---- final-transcript quality ------------------------------------------
    // Numbers a reader can compare between runs: WER/CER against a human
    // transcript of the recording, the share of human turns no caption covers,
    // how often a label names the right person, and whether the recording's own
    // vocabulary survived. Normalisation matches tools/bench/metrics.py.
    struct MetricTurn {
        double start = 0.0;
        double end = 0.0;
        QString speaker;
        QString text;
    };

    static QList<QString> metricTokens(const QString &text) {
        QString flat = text.toLower();
        flat.replace(QChar(0x0451), QChar(0x0435));
        for (QChar &c : flat)
            if (!c.isLetterOrNumber() && c != '#' && c != '.' && c != '/' && c != '\\' && c != '-'
                && c != '+' && c != '\'' && c != '_')
                c = ' ';               // same set metrics.py keeps (it's -> it's, snake_case)
        QStringList tokens = flat.split(' ', Qt::SkipEmptyParts);
        // metrics.py drops a sentence dot attached to a word ("build." ->
        // "build") but keeps dots inside identifiers ("v2.4.1").
        for (QString &token : tokens)
            while (!token.isEmpty() && (token.endsWith('.') || token.endsWith(','))) token.chop(1);
        tokens.removeAll(QString());
        return tokens;
    }

    template <typename T>
    static int edits(const QList<T> &a, const QList<T> &b) {
        QVector<int> previous(b.size() + 1), current(b.size() + 1);
        for (int j = 0; j <= b.size(); ++j) previous[j] = j;
        for (int i = 1; i <= a.size(); ++i) {
            current[0] = i;
            for (int j = 1; j <= b.size(); ++j)
                current[j] = qMin(qMin(previous[j] + 1, current[j - 1] + 1),
                                  previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1));
            previous = current;
        }
        return previous[b.size()];
    }

    static QList<QChar> metricChars(const QList<QString> &tokens) {
        QList<QChar> chars;
        for (const QString &token : tokens)
            for (QChar c : token) chars.append(c);
        return chars;
    }

    // Human turns for the whole fixture, in absolute seconds. Built from a
    // word-level annotation by tools/bench/transcript_quality.py reference.
    static QVector<MetricTurn> loadReferenceTurns(const QString &path) {
        QVector<MetricTurn> turns;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return turns;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (const QJsonValue &value : root.value(QStringLiteral("segments")).toArray()) {
            const QJsonObject turn = value.toObject();
            const QString text = turn.value(QStringLiteral("text")).toString().trimmed();
            if (text.isEmpty()) continue;
            turns.append({turn.value(QStringLiteral("start")).toDouble(),
                          turn.value(QStringLiteral("end")).toDouble(),
                          turn.value(QStringLiteral("speaker")).toString(), text});
        }
        return turns;
    }

    static bool capturePanel(MainWindow &window, const QString &path) {
        const QRect bounds = window.expanded_ ? window.geometry().united(window.editorPage_->geometry()) : window.geometry();
        QPixmap capture(bounds.size());
        capture.fill(Qt::transparent);
        QPainter painter(&capture);
        painter.drawPixmap(window.pos() - bounds.topLeft(), window.grab());
        if (window.expanded_) painter.drawPixmap(window.editorPage_->pos() - bounds.topLeft(), window.editorPage_->grab());
        painter.end();
        return capture.save(path);
    }

private slots:
    void init() {
        app_->settings()->mode = "dictation";
        app_->settings()->autoCleanEditor = false;
        app_->settings()->autoTranslate = false;
        app_->processor()->setAutoTranslate(false);
        app_->service()->setBaseUrl(app_->settings()->serviceUrl);
        app_->settings()->quickInsert = false;
        app_->settings()->autoSaveSessions = true;
        app_->settings()->transcriptResetS = 30;
        app_->settings()->compactGeometry.clear();
        app_->settings()->editorGeometry.clear();
        app_->settings()->dockPosition = "free";
        app_->processor()->reset();
    }

    void themeLoads() {
        QVERIFY2(!app_->styleSheet().isEmpty(), "The bundled theme must be applied at startup");
        QCOMPARE(app_->font().family(), QString("Inter"));
        QPushButton primary("Record");
        primary.setProperty("variant", "primary");
        primary.resize(100, 36);
        primary.show();
        QCoreApplication::processEvents();
        QCOMPARE(primary.grab().toImage().pixelColor(8, 18), QColor("#ee9a3a"));
    }

    void panelAndPreviewStayFrameless() {
        MainWindow window(app_);
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        window.showExpanded();
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(window.editorPage_->windowFlags().testFlag(Qt::FramelessWindowHint));
        window.showCompact();
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
    }

    void consecutiveTranscriptsReachTheEditor() {
        MainWindow window(app_);
        QList<QTextEdit *> edits{window.findChild<QTextEdit *>("originalText"), window.findChild<QTextEdit *>("resultText")};
        QCOMPARE(edits.size(), 2);
        app_->dictation()->finalTranscript("First sentence.", 1);
        app_->dictation()->finalTranscript("Second sentence.", 1);
        QCOMPARE(edits[1]->toPlainText(), QString("First sentence. Second sentence."));
    }

    void livePreviewAppearsWithoutStopping() {
        MainWindow window(app_);
        auto *original = window.findChild<QTextEdit *>("originalText");
        app_->dictation()->partialTranscript("Still speaking without stopping");
        QVERIFY(original->toPlainText().contains("Still speaking without stopping"));
    }

    void quickDictationAttemptsOnlyProcessedTextBeforeStop() {
        MainWindow window(app_);
        window.recording_ = window.sessionActive_ = true;
        window.reviewMode_ = false;
        window.target_.windowHandle = 1; // A closed target must retain text for retry.
        app_->dictation()->partialTranscript("Unstable words", 0);
        QTest::qWait(350);
        QVERIFY(!window.processingLabel_->text().contains("Insertion failed"));
        window.onProcessedBatch("A sentence.", "A corrected sentence.");
        QTRY_VERIFY(window.insertionFailed_);
        QCOMPARE(window.processedSoFar_, QString("A corrected sentence."));
        QVERIFY(window.recording_);
    }

    void silenceDoesNotErasePendingTranslation() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        app_->settings()->autoTranslate = true;
        app_->settings()->transcriptResetS = 1;
        MainWindow window(app_);
        window.recording_ = true;
        window.reviewMode_ = false;
        window.onFinalTranscript("A sentence.", 1, 0);
        QTRY_COMPARE(server.requests.size(), 1);
        QTest::qWait(1300);
        QCOMPARE(window.originalSoFar_, QString("A sentence."));
        server.reply(0, R"({"result":"Translated sentence."})");
        QTRY_COMPARE(window.translation_, QString("Translated sentence."));
        QCOMPARE(window.originalSoFar_, QString("A sentence."));
        window.recording_ = false;
    }

    void insertionFailureDoesNotBlockSilenceRollover() {
        app_->settings()->transcriptResetS = 1;
        MainWindow window(app_);
        window.recording_ = window.sessionActive_ = true;
        window.reviewMode_ = false;
        window.target_.windowHandle = 1;
        window.onFinalTranscript("Keep the failed insertion.", 1, 0);
        QTRY_VERIFY(window.insertionFailed_);
        QTRY_VERIFY_WITH_TIMEOUT(window.originalEdit_->toPlainText().isEmpty(), 4000);
        QVERIFY(window.recording_);
        QCOMPARE(window.preservedTranscripts_.size(), 1);
        QCOMPARE(window.preservedTranscripts_.first().record.original, QString("Keep the failed insertion."));
        window.recording_ = false;
        window.restoreTranscript(0);
        QCOMPARE(window.resultEdit_->toPlainText(), QString("Keep the failed insertion."));
        QVERIFY(window.insertionFailed_);
    }

    void failedInsertionDoesNotRequireCopyBeforeANewSession() {
        MainWindow window(app_);
        window.originalSoFar_ = "The original.";
        window.processedSoFar_ = "The processed result.";
        window.insertedText_ = "The processed";
        window.insertionFailed_ = true;
        window.updateResult();
        // Bypass hardware only in this state-transition test. The desktop
        // workflow separately verifies a real capture restart.
        app_->dictation()->sessionOpen_ = true;
        window.startDictation(false);
        QVERIFY(window.recording_);
        QVERIFY(!window.insertionFailed_);
        QCOMPARE(window.preservedTranscripts_.size(), 1);
        QCOMPARE(window.preservedTranscripts_.first().insertedText, QString("The processed"));
        window.cancelDictation();
        window.restoreTranscript(0);
        QCOMPARE(window.originalSoFar_, QString("The original."));
        QCOMPARE(window.resultEdit_->toPlainText(), QString("The processed result."));
        QCOMPARE(window.insertedText_, QString("The processed"));
    }

    void autoSaveDoesNotDependOnInsertionOrStop() {
        QTemporaryDir directory(QDir::current().filePath("autosave-XXXXXX"));
        const QString previous = app_->store()->dir();
        app_->store()->setDir(directory.path());
        const auto restore = qScopeGuard([&]() { app_->store()->setDir(previous); });
        app_->settings()->autoSaveSessions = true;
        MainWindow window(app_);
        window.recording_ = true;
        window.onFinalTranscript("An automatically saved sentence.", 1, 0);
        QDirIterator files(directory.path(), {"*.md"}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY2(files.hasNext(), "Stable text must be saved while recording, without inserting or pressing Copy");
        const QString path = files.next();
        window.resultEdit_->setPlainText("My reviewed wording.");
        QFile record(path);
        QVERIFY(record.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(record.readAll()).contains("My reviewed wording."));
        record.close();
        window.recording_ = false;
        window.sessionActive_ = window.finishing_ = true;
        window.finishSession();
        QVERIFY(record.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(record.readAll()).contains("## Final (user-confirmed)\n\nMy reviewed wording."));
    }

    void autoSaveHasALocalDefaultWithoutACustomFolder() {
        Settings settings;
        QVERIFY(settings.markdownDir.isEmpty());
        QVERIFY(settings.autoSaveSessions);
        QCOMPARE(settings.transcriptDirectory(), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/transcripts");
        settings.autoSaveSessions = false;
        QVERIFY(settings.transcriptDirectory().isEmpty());
        settings.markdownDir = "custom-transcripts";
        QCOMPARE(settings.transcriptDirectory(), QString("custom-transcripts"));
    }

    void saveFailureDoesNotBlockSilenceRollover() {
        QTemporaryDir directory(QDir::current().filePath("failed-save-XXXXXX"));
        QFile obstruction(directory.filePath("not-a-folder"));
        QVERIFY(obstruction.open(QIODevice::WriteOnly));
        obstruction.close();
        const QString previous = app_->store()->dir();
        app_->store()->setDir(obstruction.fileName());
        const auto restore = qScopeGuard([&]() { app_->store()->setDir(previous); });
        app_->settings()->transcriptResetS = 1;
        MainWindow window(app_);
        window.recording_ = true;
        window.reviewMode_ = false;
        window.onFinalTranscript("Preserve this when saving fails.", 1, 0);
        QTRY_VERIFY_WITH_TIMEOUT(window.originalSoFar_.isEmpty(), 4000);
        QVERIFY(window.recording_);
        QCOMPARE(window.preservedTranscripts_.size(), 1);
        QCOMPARE(window.preservedTranscripts_.first().reviewedText, QString("Preserve this when saving fails."));
        window.recording_ = false;
    }

    void silenceRolloverPreservesAnIncompleteFinalTranscript() {
        app_->settings()->autoCleanEditor = true;
        app_->settings()->transcriptResetS = 1;
        MainWindow window(app_);
        window.recording_ = true;
        window.reviewMode_ = false;
        window.onFinalTranscript("A finalized utterance without punctuation", 1, 0);
        QTRY_VERIFY_WITH_TIMEOUT(window.originalSoFar_.isEmpty(), 4000);
        QVERIFY(window.recording_);
        QCOMPARE(window.preservedTranscripts_.size(), 1);
        QCOMPARE(window.preservedTranscripts_.first().reviewedText, QString("A finalized utterance without punctuation"));
        window.recording_ = false;
    }

    void silenceWithoutStorageKeepsTheDocumentInHistory() {
        const QString previous = app_->store()->dir();
        app_->store()->setDir({});
        app_->settings()->transcriptResetS = 1;
        MainWindow window(app_);
        window.recording_ = true;
        window.reviewMode_ = false;
        window.onFinalTranscript("Keep this document.", 1, 0);
        QTRY_VERIFY_WITH_TIMEOUT(window.originalSoFar_.isEmpty(), 4000);
        QCOMPARE(window.preservedTranscripts_.size(), 1);
        QCOMPARE(window.preservedTranscripts_.first().record.original, QString("Keep this document."));
        window.recording_ = false;
        window.restoreTranscript(0);
        app_->store()->setDir(previous);
        QCOMPARE(window.originalSoFar_, QString("Keep this document."));
        QCOMPARE(window.resultEdit_->toPlainText(), QString("Keep this document."));
    }

    void failedProcessingIsNotFinalizedAsAnUnprocessedFragment() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TextProcessor processor(&client);
        processor.setMode("auto_correct");
        QSignalSpy fragments(&processor, &TextProcessor::fragmentRemaining);
        QSignalSpy finished(&processor, &TextProcessor::finished);
        processor.acceptFinal("Needs correction.");
        QTRY_COMPARE(server.requests.size(), 1);
        processor.finish();
        server.reply(0, R"({"detail":"Unavailable"})", false);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(fragments.size(), 0);
        QCOMPARE(processor.pendingOriginal(), QString("Needs correction."));
    }

    void reviewWaitsForRecognitionAndProcessingAndIgnoresRepeatedToggles() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        app_->settings()->autoCleanEditor = true;
        MainWindow window(app_);
        window.recording_ = window.sessionActive_ = true;
        window.reviewMode_ = true;
        auto *controller = app_->dictation();
        controller->sessionOpen_ = true;
        controller->pendingFinals_ = 1;
        controller->requestFinal(0, QVector<float>(4096), 1);
        QTRY_COMPARE(server.requests.size(), 1);
        const auto generation = controller->generation_;
        window.toggleDictation(true);
        window.toggleDictation(false);
        window.toggleDictation(true);
        QVERIFY(window.finishing_ && !window.recording_);
        QCOMPARE(controller->generation_, generation);
        QVERIFY(!controller->start());
        QCOMPARE(controller->generation_, generation);
        QVERIFY(!window.expanded_);
        server.reply(0, R"({"text":"Final sentence."})");
        QTRY_COMPARE(server.requests.size(), 2);
        QVERIFY(window.finishing_);
        QVERIFY(!window.expanded_);
        window.toggleDictation(false);
        server.reply(1, R"({"result":"Final corrected sentence."})");
        QTRY_VERIFY(!window.finishing_);
        QVERIFY(window.expanded_);
        QCOMPARE(window.resultEdit_->toPlainText(), QString("Final corrected sentence."));
        QVERIFY(window.insertedText_.isEmpty());
    }

    void threadHotkeyMessagesReachTheHandler() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires Windows messages");
        HotkeyManager hotkeys;
        QVERIFY(hotkeys.registerHotkey("Ctrl+Alt+Shift+J"));
        QSignalSpy triggered(&hotkeys, &HotkeyManager::triggered);
        QVERIFY(PostThreadMessage(GetCurrentThreadId(), WM_HOTKEY, 1, 0));
        QTRY_COMPARE(triggered.size(), 1);
        QCOMPARE(triggered[0][0].toString(), QString("Ctrl+Alt+Shift+J"));
    }

    void desktopInsertionIntoFocusedTarget() {
        if (!qEnvironmentVariableIsSet("VOICEPANEL_TARGET_TEST")) QSKIP("Opt-in Codex, Telegram or browser insertion check");
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        app_->settings()->autoCleanEditor = true;
        app_->settings()->transcriptResetS = 0;
        app_->settings()->insertHotkey = "Ctrl+Alt+D";
        MainWindow window(app_);
        app_->window_ = &window;
        window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        app_->wireHotkeys();
        const auto cleanup = qScopeGuard([this]() {
            app_->window_ = nullptr;
            app_->hotkeys()->unregisterAll();
        });
        QVERIFY(app_->hotkeys()->registerHotkey("Ctrl+Alt+D"));
        window.showCompact();
        auto stage = [](const char *value) {
            QFile file("target-insertion-stage.txt");
            if (file.open(QIODevice::WriteOnly)) file.write(value);
        };
        QSignalSpy captures(app_->dictation()->capture_, &WasapiCapture::started);
        stage("start");
        QTRY_VERIFY_WITH_TIMEOUT(window.recording_, 90000);
        QTRY_COMPARE(captures.size(), 1);
        app_->dictation()->finalTranscript("First target sentence.", 1, 0);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"result":"VoicePanel insertion check."})");
        QTRY_COMPARE_WITH_TIMEOUT(window.insertedText_, QString("VoicePanel insertion check."), 10000);
        app_->dictation()->finalTranscript("Second target sentence.", 1, 1);
        QTRY_COMPARE(server.requests.size(), 2);
        stage("stop-pending");
        QTRY_VERIFY_WITH_TIMEOUT(window.finishing_ && !window.recording_, 90000);
        server.reply(1, R"({"result":"Second sentence in order."})");
        QTRY_VERIFY_WITH_TIMEOUT(!window.finishing_, 10000);
        QCOMPARE(window.insertedText_, QString("VoicePanel insertion check. Second sentence in order."));
        QVERIFY(!window.insertionFailed_);
        QCOMPARE(captures.size(), 1);
        QTest::qWait(500);
        stage("verify-target");
        const QString confirmation = qEnvironmentVariable("VOICEPANEL_TARGET_CONTINUE");
        QVERIFY2(!confirmation.isEmpty(), "Set VOICEPANEL_TARGET_CONTINUE and confirm the actual target text before completing this test");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(confirmation), 90000);
        stage("complete");
    }

    void desktopDictationWorkflow() {
        if (!qEnvironmentVariableIsSet("VOICEPANEL_DESKTOP_TEST")) QSKIP("Opt-in external text-field workflow");
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        app_->settings()->autoCleanEditor = true;
        app_->settings()->reviewBeforeInsert = true; // The explicit quick shortcut takes precedence.
        app_->settings()->transcriptResetS = 1;
        app_->settings()->insertHotkey = "Ctrl+Alt+D";
        app_->settings()->insertHotkey2 = "Ctrl+Alt+R";
        MainWindow window(app_);
        app_->window_ = &window;
        // Keep both windows frameless while exposing them to desktop automation.
        window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        const auto previewFlags = window.editorPage_->windowFlags();
        window.editorPage_->setParent(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        window.editorPage_->setWindowTitle("VoicePanel dictation review test");
        app_->wireHotkeys();
        const auto cleanup = qScopeGuard([this, &window, previewFlags]() {
            window.editorPage_->setParent(&window, previewFlags);
            app_->window_ = nullptr;
            app_->hotkeys()->unregisterAll();
        });
        QVERIFY(app_->hotkeys()->registerHotkey("Ctrl+Alt+D"));
        QVERIFY(app_->hotkeys()->registerHotkey("Ctrl+Alt+R"));
        window.showCompact();
        auto stage = [](const char *name) {
            QFile file("desktop-dictation-stage.txt");
            if (file.open(QIODevice::WriteOnly)) file.write(name);
            qInfo("Desktop stage: %s", name);
        };
        auto *controller = app_->dictation();
        QSignalSpy captures(controller->capture_, &WasapiCapture::started);
        stage("quick-start");
        QTRY_VERIFY_WITH_TIMEOUT(window.recording_, 90000);
        QVERIFY(!window.reviewMode_);
        QVERIFY(window.target_.windowTitle.contains("voicepanel-dictation-test"));
        QTRY_COMPARE(captures.size(), 1);
        controller->partialTranscript("Unstable words", 0);
        QTest::qWait(200);
        QVERIFY(window.insertedText_.isEmpty());
        controller->pendingFinals_ = 2;
        controller->segmentId_ = 2;
        controller->requestFinal(0, QVector<float>(4096), 1);
        controller->requestFinal(1, QVector<float>(4096), 1);
        QTRY_COMPARE(server.requests.size(), 2);
        server.reply(1, R"({"text":"Second sentence."})");
        QTest::qWait(100);
        QVERIFY(window.originalSoFar_.isEmpty());
        server.reply(0, R"({"text":"First sentence."})");
        QTRY_COMPARE(server.requests.size(), 3);
        server.reply(2, R"({"result":"First corrected."})");
        QTRY_COMPARE(window.insertedText_, QString("First corrected."));
        QTRY_COMPARE(server.requests.size(), 4);
        QTest::qWait(1300);
        QCOMPARE(window.originalSoFar_, QString("First sentence. Second sentence."));
        QVERIFY(controller->active());
        QCOMPARE(captures.size(), 1);
        stage("quick-stop-pending");
        QTRY_VERIFY_WITH_TIMEOUT(window.finishing_, 90000);
        stage("quick-draining");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(qEnvironmentVariable("VOICEPANEL_DESKTOP_CONTINUE") + "-quick"), 90000);
        QVERIFY(!window.recording_ && window.finishing_);
        QCOMPARE(captures.size(), 1);
        server.reply(3, R"({"result":"Second corrected."})");
        QTRY_VERIFY(!window.finishing_);
        QCOMPARE(window.insertedText_, QString("First corrected. Second corrected."));
        stage("repeat-start");
        QTRY_VERIFY_WITH_TIMEOUT(window.recording_, 90000);
        QTRY_COMPARE(captures.size(), 2);
        controller->finalTranscript("Next session.", 1, 0);
        QTRY_COMPARE(server.requests.size(), 5);
        server.reply(4, R"({"result":"Next corrected."})");
        QTRY_COMPARE(window.insertedText_, QString("Next corrected."));
        stage("repeat-stop");
        QTRY_VERIFY_WITH_TIMEOUT(!window.recording_ && !window.finishing_, 90000);
        stage("review-start");
        QTRY_VERIFY_WITH_TIMEOUT(window.recording_, 90000);
        QVERIFY(window.reviewMode_);
        QTRY_COMPARE(captures.size(), 3);
        controller->finalTranscript("Review sentence.", 1, 0);
        QTRY_COMPARE(server.requests.size(), 6);
        stage("review-stop-pending");
        QTRY_VERIFY_WITH_TIMEOUT(window.finishing_, 90000);
        stage("review-draining");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(qEnvironmentVariable("VOICEPANEL_DESKTOP_CONTINUE") + "-review"), 90000);
        QVERIFY(window.insertedText_.isEmpty());
        QCOMPARE(captures.size(), 3);
        server.reply(5, R"({"result":"Review corrected."})");
        QTRY_VERIFY(!window.finishing_);
        QVERIFY(window.expanded_);
        QVERIFY(window.insertedText_.isEmpty());
        stage("review-confirm");
        QTRY_COMPARE_WITH_TIMEOUT(window.insertedText_, QString("Review corrected."), 90000);
        QTest::qWait(500);
        // Force an unavailable target, then verify retry inserts only the suffix.
        const auto handle = window.target_.windowHandle;
        window.target_.windowHandle = 1;
        window.resultEdit_->setPlainText("Review corrected. Retry suffix.");
        window.insertResult();
        QVERIFY(window.insertionFailed_);
        QCOMPARE(window.insertedText_, QString("Review corrected."));
        window.startDictation(false);
        QVERIFY(window.recording_);
        window.cancelDictation();
        window.restoreTranscript(window.preservedTranscripts_.size() - 1);
        window.target_.windowHandle = handle;
        stage("retry-confirm");
        QTRY_COMPARE_WITH_TIMEOUT(window.insertedText_, QString("Review corrected. Retry suffix."), 90000);
        QVERIFY(!window.insertionFailed_);
        QTest::qWait(500);
        stage("complete");
    }

    void incompleteTranscriptIsVisibleBeforeStop() {
        app_->settings()->mode = "auto_correct";
        app_->settings()->autoCleanEditor = true;
        MainWindow window(app_);
        app_->dictation()->finalTranscript("A thought that is still continuing", 1);
        auto *result = window.findChild<QTextEdit *>("resultText");
        QCOMPARE(result->toPlainText(), QString("A thought that is still continuing"));
    }

    void workflowDoesNotRequireModeOrTopicSelection() {
        MainWindow window(app_);
        QVERIFY(window.findChildren<QComboBox *>().isEmpty());
        SettingsDialog dialog(app_);
        for (auto *combo : dialog.findChildren<QComboBox *>())
            QVERIFY(combo->findData("programming") < 0);
        QVERIFY(dialog.findChild<QPlainTextEdit *>("userContext"));
    }

    void sideDockUsesAVerticalRail() {
        MainWindow window(app_);
        window.show();
        window.setDockPosition("left");
        QVERIFY(window.width() <= 80);
        QVERIFY(window.height() > window.width());
        window.showExpanded();
        QVERIFY(window.width() <= 80);
        QVERIFY(window.editorPage_->width() >= 640);
        window.showCompact();
        QVERIFY(window.width() <= 80);
        window.setDockPosition("right");
        QVERIFY(window.width() <= 80);
        QVERIFY(window.grab().save("panel-vertical.png"));
        const QSize railSize = window.size();
        window.setDockPosition("top");
        QCOMPARE(window.size(), railSize.transposed());
    }

    void pendingSpeculationIsReusedAtPauseWithoutStop() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        DictationController controller(&client);
        controller.sessionOpen_ = true;
        controller.inSpeech_ = true;
        controller.revision_ = 3;
        controller.segment_.fill(0.1f, 4096);
        QSignalSpy finals(&controller, &DictationController::finalTranscript);
        QSignalSpy ended(&controller, &DictationController::sessionEnded);
        controller.launchSpeculative();
        QTRY_COMPARE(server.requests.size(), 1);
        controller.finishSegment(true);
        server.reply(0, R"({"text":"A complete thought."})");
        QTRY_COMPARE_WITH_TIMEOUT(finals.size(), 1, 1000);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(ended.size(), 0);
        QVERIFY(controller.sessionOpen_);
    }

    void periodicSpeculationWaitsForTheSegmentToGrow() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        DictationController controller(&client);
        controller.sessionOpen_ = controller.inSpeech_ = true;
        controller.speechFrames_ = 200;
        controller.revision_ = 200;
        controller.lastSpeculateFrame_ = 160;      // last request covered 5.1 s
        controller.segment_.fill(0.1f, 230 * Vad::kFrameSamples);
        controller.maybeSpeculate();               // +70 frames: past the fixed 63, below 160 / 2
        QVERIFY(!controller.speculateInFlight_);
        controller.segment_.fill(0.1f, 240 * Vad::kFrameSamples);
        controller.maybeSpeculate();               // +80 frames: half the last request
        QVERIFY(controller.speculateInFlight_);
        QTRY_COMPARE(server.requests.size(), 1);
        controller.cancel();
    }

    void pauseSpeculationIsNotDelayedByTheGrowthRule() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        DictationController controller(&client);
        controller.sessionOpen_ = controller.inSpeech_ = true;
        controller.speechFrames_ = controller.revision_ = 200;
        controller.lastSpeculateFrame_ = 200;
        controller.silenceFrames_ = 6;             // pause candidate
        controller.segment_.fill(0.1f, 206 * Vad::kFrameSamples);
        controller.maybeSpeculate();
        QTRY_COMPARE(server.requests.size(), 1);
        controller.cancel();
    }

    void warmupCarriesModeAndContext() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        client.setContext("Glossary: UCLASS");
        client.warmup("auto_correct", "general");
        QTRY_COMPARE(server.requests.size(), 1);
        const auto request = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(request.value("mode").toString(), QString("auto_correct"));
        QCOMPARE(request.value("context").toString(), QString("Glossary: UCLASS"));
        server.reply(0, R"({"scheduled":["asr","llm"]})");
    }

    void continuousCorrectionReachesPreviewBeforeStop() {
        TestServer server;
        app_->settings()->autoCleanEditor = true;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        window.recording_ = true;
        app_->dictation()->sessionOpen_ = true;
        auto *original = window.findChild<QTextEdit *>("originalText");
        auto *result = window.findChild<QTextEdit *>("resultText");
        app_->dictation()->partialTranscript("This function", 0);
        QVERIFY(original->toPlainText().contains("This function"));
        app_->dictation()->finalTranscript("This function", 1, 0);
        QCOMPARE(result->toPlainText(), QString("This function"));
        QCOMPARE(server.requests.size(), 0);
        app_->dictation()->finalTranscript("return values.", 1, 1);
        QTRY_COMPARE(server.requests.size(), 1);
        const auto request = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(request.value("mode").toString(), QString("correct_en"));
        QVERIFY(request.value("context").toString().contains("game design"));
        server.reply(0, R"({"result":"This function returns values."})");
        QTRY_COMPARE(result->toPlainText(), QString("This function returns values."));
        QVERIFY(window.recording_);
        QVERIFY(app_->dictation()->sessionOpen_);
        QVERIFY(!result->extraSelections().isEmpty());
        window.recording_ = false;
    }

    void realBackendsUpdateBothPanesBeforeStop() {
        if (!qEnvironmentVariableIsSet("VOICEPANEL_LIVE_TEST")) QSKIP("Opt-in local inference test");
        QFile fixture("live-fixture.wav");
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QByteArray wav = fixture.readAll();
        QVERIFY(wav.startsWith("RIFF"));
        QVERIFY(wav.mid(36, 4) == "data");
        app_->settings()->autoCleanEditor = true;
        app_->service()->setBaseUrl(qEnvironmentVariable("VOICEPANEL_TEST_SERVICE_URL", "http://127.0.0.1:8080"));
        MainWindow window(app_);
        bool asrReady = false, llmReady = false;
        app_->service()->transcribe(wav, "default", "auto", [&asrReady](bool ok, QString, QString) { asrReady = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(asrReady, 120000);
        app_->service()->processText("This is a test.", "correct_en", "general", {}, {},
            [&llmReady](bool ok, QString, QString, QString) { llmReady = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(llmReady, 120000);
        auto *controller = app_->dictation();
        controller->cancel();
        controller->sessionOpen_ = true;
        controller->segmentId_ = controller->nextFinalId_ = 0;
        controller->requestedRevision_ = -1;
        DictationParams params;
        controller->setParams(params);
        QSignalSpy previews(controller, &DictationController::partialTranscript);
        QSignalSpy finals(controller, &DictationController::finalTranscript);
        QSignalSpy batches(app_->processor(), &TextProcessor::processedBatch);
        QSignalSpy ended(controller, &DictationController::sessionEnded);
        window.recording_ = true;
        window.showExpanded();
        const auto started = QDateTime::currentMSecsSinceEpoch();
        for (int offset = 44; offset + 1024 <= wav.size(); offset += 1024) {
            QVector<float> frame(512);
            for (int i = 0; i < 512; ++i) {
                const int value = quint8(wav[offset + i * 2]) | (int(quint8(wav[offset + i * 2 + 1])) << 8);
                frame[i] = qint16(value) / 32768.0f;
            }
            controller->processBlock(frame);
            QTest::qWait(32);
        }
        QTRY_VERIFY_WITH_TIMEOUT(finals.size() >= 2, 90000);
        QTRY_VERIFY_WITH_TIMEOUT(batches.size() >= 2 && !app_->processor()->busy(), 90000);
        const QJsonObject evidence{{"preview_before_stop", !previews.isEmpty()}, {"final_before_stop", finals.size() >= 2},
            {"correction_before_stop", batches.size() >= 2}, {"session_still_open", window.recording_ && controller->sessionOpen_}, {"previews", previews.size()},
            {"finals", finals.size()}, {"corrections", batches.size()},
            {"elapsed_ms", double(QDateTime::currentMSecsSinceEpoch() - started)},
            {"original", window.originalEdit_->toPlainText()}, {"result", window.resultEdit_->toPlainText()}};
        QFile report("live-continuous-results.json");
        QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(evidence).toJson());
        report.close();
        QVERIFY(!window.originalEdit_->toPlainText().isEmpty());
        QVERIFY(!window.resultEdit_->toPlainText().isEmpty());
        QVERIFY(!previews.isEmpty());
        QCOMPARE(ended.size(), 0);
        QVERIFY(window.recording_ && controller->sessionOpen_);
        QVERIFY(capturePanel(window, "live-continuous-preview.png"));
        window.recording_ = false;
        controller->cancel();
    }

    void finalizingOlderSpeechKeepsTheNewPreview() {
        MainWindow window(app_);
        app_->dictation()->partialTranscript("Old preview", 0);
        app_->dictation()->partialTranscript("New preview", 1);
        app_->dictation()->finalTranscript("Old final.", 1, 0);
        auto *original = window.findChild<QTextEdit *>("originalText");
        QVERIFY(original->toPlainText().contains("New preview"));
        QVERIFY(!original->toPlainText().contains("Old preview"));
        app_->dictation()->finalTranscript("New final.", 1, 1);
        QCOMPARE(original->toPlainText(), QString("Old final. New final."));
    }

    void disablingAutocorrectionMakesNoLlmRequests() {
        TestServer server;
        app_->settings()->autoCleanEditor = false;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        app_->dictation()->finalTranscript("Keep this sentence unchanged.", 1);
        QCOMPARE(window.findChild<QTextEdit *>("resultText")->toPlainText(), QString("Keep this sentence unchanged."));
        QTest::qWait(30);
        QCOMPARE(server.requests.size(), 0);
        SettingsDialog dialog(app_);
        auto *checkbox = dialog.findChild<QCheckBox *>("autoCorrection");
        QVERIFY(checkbox);
        QVERIFY(!checkbox->isChecked());
    }

    void oneContextDocumentSurvivesReloadAndReachesBothApis() {
        QTemporaryDir directory(QDir::current().filePath("context-test-XXXXXX"));
        Settings settings;
        settings.contextDocument = directory.filePath("context.md");
        settings.context = "AI prompts, programming, Godot, Unity, Unreal Engine, game development, game design.";
        QVERIFY(settings.saveContext());
        settings.context.clear();
        settings.loadContext();
        QVERIFY(settings.context.contains("game design"));
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        client.setContext(settings.context);
        client.transcribe("test audio", "default", "auto", [](bool, QString, QString) {});
        QTRY_COMPARE(server.requests.size(), 1);
        QVERIFY(server.requests[0]->property("body").toByteArray().contains(settings.context.toUtf8()));
        server.reply(0, R"({"text":"Test sentence."})");
        client.processText("Test sentence.", "correct_en", "general", {}, {}, [](bool, QString, QString, QString) {});
        QTRY_COMPARE(server.requests.size(), 2);
        const auto payload = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("context").toString(), settings.context);
        server.reply(1, R"({"result":"Test sentence."})");
    }

    void individualCorrectionsCanBeAcceptedAndRejected() {
        TestServer server;
        app_->settings()->autoCleanEditor = true;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        app_->dictation()->finalTranscript("This are a test.", 1);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"result":"This is the test."})");
        auto *result = window.findChild<QTextEdit *>("resultText");
        QTRY_COMPARE(result->toPlainText(), QString("This is the test."));
        QCOMPARE(window.corrections_.size(), 2);
        window.showExpanded();
        QCoreApplication::processEvents();
        QVERIFY(capturePanel(window, "comparison-corrections.png"));
        window.findChild<QPushButton *>("acceptCorrection")->click();
        QCOMPARE(result->toPlainText(), QString("This is the test."));
        QVERIFY(window.corrections_.isEmpty());
    }

    void translatedOutputHasNoCorrectionReview() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        window.showExpanded();
        window.autoTranslateBox_->setChecked(true);
        app_->dictation()->finalTranscript(QString::fromUtf8("Привет, мир."), 1);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"result":"Hello, world."})");
        QTRY_COMPARE(window.resultEdit_->toPlainText(), QString("Hello, world."));
        QVERIFY(window.corrections_.isEmpty());
        QVERIFY(window.resultEdit_->extraSelections().isEmpty());
        QVERIFY(window.originalEdit_->extraSelections().isEmpty());
        QVERIFY(!window.acceptButton_->isEnabled());
        QVERIFY(!window.rejectButton_->isEnabled());
        QVERIFY(!window.reviewBar_->isVisible());
        window.autoTranslateBox_->setChecked(false);
        QTest::qWait(20);
    }

    void restoredHistoryCanRejectAllCorrections() {
        MainWindow window(app_);
        window.originalSoFar_ = "This are a test.";
        window.processedSoFar_ = "This is the test.";
        window.updateOriginal();
        window.updateResult();
        QCOMPARE(window.corrections_.size(), 2);
        window.preserveTranscript();
        window.restoreTranscript(0);
        window.rejectButton_->click();
        QCOMPARE(window.resultEdit_->toPlainText(), QString("This are a test."));
        QVERIFY(window.corrections_.isEmpty());
    }

    void correctionsCanBeDecidedWhileRecordingAndSurviveLaterBatches() {
        TestServer server;
        app_->settings()->autoCleanEditor = true;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        window.showExpanded();
        window.recording_ = true;
        app_->dictation()->finalTranscript("This are a test.", 1, 0);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"result":"This is the test."})");
        QTRY_COMPARE(window.corrections_.size(), 2);
        QVERIFY(!window.resultEdit_->extraSelections().isEmpty());
        auto *accept = window.findChild<QPushButton *>("acceptCorrection");
        auto *reject = window.findChild<QPushButton *>("rejectCorrection");
        QVERIFY(accept->isEnabled());
        QVERIFY(reject->isEnabled());
        reject->click();
        QCOMPARE(window.resultEdit_->toPlainText(), QString("This are a test."));
        QVERIFY(window.corrections_.isEmpty());
        QVERIFY(window.recording_);
        // A later batch must keep both decisions and still reach the pane.
        app_->dictation()->finalTranscript("Another sentence.", 1, 1);
        QTRY_COMPARE(server.requests.size(), 2);
        server.reply(1, R"({"result":"Another sentence."})");
        QTRY_COMPARE(window.resultEdit_->toPlainText(),
                     QString("This are a test. Another sentence."));
        QVERIFY(window.corrections_.isEmpty());
        QVERIFY(window.resultEdit_->extraSelections().isEmpty());
        window.recording_ = false;
    }

    void serverSettingsWindowReadsAndSavesComputeChoices() {
        TestServer server;
        ServerSettingsDialog dialog(server.url());
        QTRY_COMPARE(server.requests.size(), 1);
        const QByteArray snapshot = R"({"groups":[
            {"id":"llm","label":"Translation and correction","device":"cpu","available":["cpu","gpu"],"running":true,"busy":false},
            {"id":"clone","label":"Cloned voice TTS","device":"gpu","available":["cpu","gpu"],"running":false,"busy":false},
            {"id":"main","label":"Recognition and built-in voices","device":"cpu","available":["cpu"],"running":true,"busy":false}]})";
        server.reply(0, snapshot);
        QTRY_VERIFY(dialog.findChild<QComboBox *>("device-llm"));
        auto *llm = dialog.findChild<QComboBox *>("device-llm");
        auto *save = dialog.findChild<QPushButton *>("saveServerCompute");
        auto *status = dialog.findChild<QLabel *>("serverComputeStatus");
        QVERIFY(llm && save && status);
        QTRY_COMPARE(llm->currentData().toString(), QString("cpu"));
        QVERIFY(!save->isEnabled());          // a shown value is not a saved value
        QCOMPARE(dialog.findChild<QComboBox *>("device-main")->count(), 1);
        llm->setCurrentIndex(llm->findData("gpu"));
        QVERIFY(save->isEnabled());
        save->click();
        QTRY_COMPARE(server.requests.size(), 2);
        const auto body = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(body.value("devices").toObject().value("llm").toString(), QString("gpu"));
        QCOMPARE(body.value("devices").toObject().value("main").toString(), QString("cpu"));
        // A busy service refuses the change; the window keeps showing its choices.
        server.reply(1, R"({"detail":"service is busy: llm: a request is in progress"})", false);
        QTRY_VERIFY(status->text().contains("busy"));
        QCOMPARE(llm->currentData().toString(), QString("cpu"));
        QVERIFY(!save->isEnabled());
        dialog.resize(560, 420);
        dialog.show();
        QCoreApplication::processEvents();
        QVERIFY(dialog.grab().save("server-settings.png"));
    }

    void spellCheckerSuggestsWithoutReplacing() {
        SpellingChecker checker;
        QVERIFY2(checker.available(), "the Windows spell checker must be available");
        const auto hints = checker.check("helo world", "en");
        QVERIFY(!hints.isEmpty());
        QCOMPARE(hints.first().word, QString("helo"));
        QVERIFY2(hints.first().suggestions.contains("hello"),
                 qPrintable(hints.first().suggestions.join(", ")));
        // Identifiers, paths and versions are not the checker's business.
        QVERIFY(checker.check("loadUserConfig src/config/service.py v2.4.1", "en").isEmpty());
        const auto russian = checker.check(QString::fromUtf8("привет, ето тест"), "ru");
        QVERIFY(!russian.isEmpty());
        QCOMPARE(russian.first().word, QString::fromUtf8("ето"));
    }

    void spellCheckerFiltersKnownWords() {
        // filterKnown is pure: it must drop hints for words the user listed in
        // their terminology, no matter case or ё/е, and keep the rest.
        const QStringList known = {QString::fromUtf8("Droppo"), QString::fromUtf8("дропбокс")};
        QList<SpellingHint> hints = {
            {QString::fromUtf8("дропбокс"), {}},
            {QString("droppo"), {"drop"}},
            {QString("helo"), {"hello"}},
        };
        const auto out = SpellingChecker().filterKnown(hints, known);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first().word, QString("helo"));
        // An empty list changes nothing.
        QCOMPARE(SpellingChecker().filterKnown(hints, {}).size(), hints.size());
    }

    void contextTerminologySectionBecomesTheDictionary() {
        const QString doc = QStringLiteral(
            "# Example speech context\n"
            "Free text with words like dropbox is only prompt context.\n"
            "\n"
            "## Terminology\n"
            "A plain comment line is not a term.\n"
            "- dropbox\n"
            "- дропбокс\n"
            "• Godot\n"
            "- Dropbox\n"   // duplicate, different case
            "\n"
            "## Other section\n"
            "- not a term\n");
        const QStringList terms = Settings::termsFromContext(doc);
        QCOMPARE(terms, QStringList({QStringLiteral("dropbox"), QString::fromUtf8("дропбокс"), QStringLiteral("Godot")}));
        // No section: nothing is a term, even if the free text mentions words.
        QVERIFY(Settings::termsFromContext(QStringLiteral("dropbox everywhere, no heading")).isEmpty());
        QVERIFY(Settings::termsFromContext(QString()).isEmpty());
    }

    void checkBundleReportsEveryPartWithoutStartingTheApp() {
        // --check-bundle: no window, tray or microphone; with the fake service
        // answering /health the whole packaged app must report OK.
        TestServer server;
        app_->settings()->serviceUrl = server.url();
        app_->service()->setBaseUrl(server.url());
        QCOMPARE(app_->runCheckBundle(), 0);
        // A dead service is a FAIL, not a crash and not a hang.
        app_->settings()->serviceUrl = "http://127.0.0.1:1";
        QCOMPARE(app_->runCheckBundle(), 1);
    }

    void silenceStartsANewTranscriptWithoutStoppingTheMicrophone() {
        const int previous = app_->settings()->transcriptResetS;
        app_->settings()->transcriptResetS = 1;
        app_->settings()->autoTranslate = true;
        MainWindow window(app_);
        window.recording_ = true;                 // the microphone is on
        window.reviewMode_ = false;
        window.originalSoFar_ = "Проверка.";
        window.processedSoFar_ = "Check.";
        window.insertedText_ = "Check.";
        window.sessionStartedAt_ = QDateTime::currentDateTime();
        window.sessionId_ = "silence-rollover";
        window.updateResult();
        window.updateOriginal();
        QVERIFY(!window.originalEdit_->toPlainText().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(window.originalEdit_->toPlainText().isEmpty(), 5000);
        QVERIFY2(window.recording_, "the microphone must keep running through the reset");
        bool saved = false;
        QDirIterator records(app_->store()->dir(), {"*silence-rollover.md"}, QDir::Files, QDirIterator::Subdirectories);
        while (records.hasNext()) {
            QFile record(records.next());
            QVERIFY(record.open(QIODevice::ReadOnly));
            const QString contents = QString::fromUtf8(record.readAll());
            saved |= contents.contains("Проверка.") && contents.contains("## Translation") && contents.contains("Check.");
        }
        QVERIFY(saved);
        app_->settings()->transcriptResetS = previous;
    }

    void comparisonPanesHaveEqualHeightAndLinkedScrolling() {
        MainWindow window(app_);
        window.showExpanded();
        window.editorPage_->resize(640, 600);
        auto *original = window.findChild<QTextEdit *>("originalText");
        auto *result = window.findChild<QTextEdit *>("resultText");
        QString text;
        for (int i = 0; i < 200; ++i) text += QString("Line %1 of the transcript.\n").arg(i);
        original->setPlainText(text);
        result->setPlainText(text);
        QCoreApplication::processEvents();
        QCOMPARE(original->height(), result->height());
        QVERIFY(original->height() > 300);
        auto *left = original->verticalScrollBar(), *right = result->verticalScrollBar();
        left->setValue(left->maximum() / 2);
        QCOMPARE(right->value(), qRound(double(left->value()) * right->maximum() / left->maximum()));
        right->setValue(right->maximum());
        QCOMPARE(left->value(), left->maximum());
        QVERIFY(capturePanel(window, "comparison-scroll.png"));
    }

    void previewHasOneToggleAndNoPlaybackOrMeter() {
        MainWindow window(app_);
        QCOMPARE(window.findChildren<QPushButton *>("previewButton").size(), 1);
        QVERIFY(!button(window, "Listen"));
        QVERIFY(!button(window, "Collapse"));
        for (auto *widget : window.findChildren<QWidget *>())
            QVERIFY(QString(widget->metaObject()->className()) != "VoicePanel::AudioMeter");
        auto *record = window.findChild<QPushButton *>("recordButton");
        const auto idle = record->icon().pixmap(24).toImage();
        app_->dictation()->levelChanged(0.8);
        QCOMPARE(record->icon().pixmap(24).toImage(), idle);
        window.recording_ = true;
        app_->dictation()->levelChanged(0.9);
        QVERIFY(record->icon().pixmap(24).toImage() != idle);
        window.recording_ = false;
    }

    void translationIsVisibleAndRunsOnlyOnRequest() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        window.showExpanded();
        auto *automatic = window.findChild<QCheckBox *>("autoTranslation");
        auto *translate = window.findChild<QPushButton *>("translateText");
        QVERIFY(automatic);
        QVERIFY(translate);
        QVERIFY(!automatic->isChecked());
        app_->dictation()->finalTranscript(QString::fromUtf8("Привет, мир."), 1);
        QTest::qWait(20);
        QCOMPARE(server.requests.size(), 0);
        automatic->setChecked(true);
        QTRY_COMPARE(server.requests.size(), 1);
        auto body = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(body["mode"].toString(), QString("translate_ru_en"));
        server.reply(0, R"({"result":"Hello, world."})");
        QTRY_COMPARE(window.resultEdit_->toPlainText(), QString("Hello, world."));
        automatic->setChecked(false);
        QCOMPARE(window.resultEdit_->toPlainText(), QString::fromUtf8("Привет, мир."));
        window.resultEdit_->setPlainText("My manual revision.");
        automatic->setChecked(true);
        QCOMPARE(window.resultEdit_->toPlainText(), QString("My manual revision."));
        automatic->setChecked(false);
        translate->click();
        QTRY_COMPARE(server.requests.size(), 2);
        body = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(body["text"].toString(), QString("My manual revision."));
        server.reply(1, QString::fromUtf8("{\"result\":\"Моя правка.\"}").toUtf8());
        QTRY_COMPARE(window.resultEdit_->toPlainText(), QString::fromUtf8("Моя правка."));
    }

    void serviceIndicatorSurvivesPanelAndStopsOnlyOnRequest() {
        TestServer server;
        ServiceTray indicator(server.url());
        QTRY_VERIFY(indicator.stop_->isEnabled());
        QVERIFY(indicator.icon_.isVisible());
        QVERIFY(indicator.icon_.toolTip().contains("Running"));
        QVERIFY2(indicator.menu_->findChild<QAction *>("serverSettings"),
                 "the service tray must open the server settings window");
        {
            MainWindow panel(app_);
            panel.show();
            panel.hide();
        }
        QVERIFY(indicator.icon_.isVisible());
        QCOMPARE(server.requests.size(), 0);
        indicator.setRunning(false);
        QVERIFY(indicator.icon_.toolTip().contains("Offline"));
        QVERIFY(!indicator.stop_->isEnabled());
        QVERIFY2(indicator.start_->isEnabled(),
                 "the tray must be able to start a stopped service");
        indicator.setRunning(true);
        QVERIFY(!indicator.start_->isEnabled());
        QSignalSpy stopped(&indicator, &ServiceTray::stopped);
        indicator.stop_->trigger();
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"status":"shutting_down"})");
        QTRY_COMPARE(stopped.size(), 1);
    }

    void voiceDesignFollowsSelectedModelCapabilities() {
        VoiceDesignWidget design;
        const QJsonObject schema{{"gender", QJsonArray{"male", "female"}},
                                 {"pitch", QJsonArray{"low pitch", "high pitch"}}};
        design.setModels(QJsonArray{
            QJsonObject{{"id", "omni"}, {"speech_parameters", QJsonObject{
                {"instructions", true}, {"speed", false}, {"voice_design", schema}, {"options", QJsonArray{"steps", "seed"}}}}},
            QJsonObject{{"id", "piper"}, {"speech_parameters", QJsonObject{{"instructions", false}, {"speed", true}}}},
        });
        design.setInstructions("female, low pitch");
        design.setOptions(QJsonObject{{"steps", 16}, {"seed", 42}});
        design.setModel("omni");
        QVERIFY(!design.isHidden());
        QCOMPARE(design.instructions(), QString("female, low pitch"));
        QCOMPARE(design.options().value("steps").toInt(), 16);
        auto *gender = design.findChild<QComboBox *>("voiceDesign_gender");
        QVERIFY(gender);
        gender->setCurrentIndex(gender->findData("male"));
        QCOMPARE(design.instructions(), QString("male, low pitch"));
        design.setModel("piper");
        QVERIFY(design.isHidden());
        QVERIFY(design.instructions().isEmpty());
        QVERIFY(design.options().isEmpty());
        QCOMPARE(design.savedInstructions(), QString("male, low pitch"));
        QCOMPARE(design.savedOptions().value("seed").toInt(), 42);
        design.setModel("omni");
        QCOMPARE(design.instructions(), QString("male, low pitch"));
    }

    void selectingSameModelForACloneClearsPreviousDesign() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        player.setModel("omnivoice-q8");
        player.setInstructions("female");
        player.setSpeechOptions(QJsonObject{{"seed", 42}});
        player.setModel("omnivoice-q8");
        player.setVoiceProfile("saved-clone");
        player.speak("This is a cloned voice.");
        QTRY_COMPARE(server.requests.size(), 1);
        const auto payload = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("voice").toString(), QString("saved-clone"));
        QVERIFY(!payload.contains("instructions"));
        QVERIFY(!payload.contains("options"));
        player.stop();
        server.reply(0, "cancelled");
    }

    void speechDesignReachesApiAndDoesNotLeakIntoBuiltinSpeech() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        player.setModel("omnivoice-q8");
        player.setVoiceProfile("saved-clone");
        player.setSpeed(1.5);
        player.setSpeechParameters(QJsonObject{{"speed", false}, {"voice_cloning", true}});
        player.setInstructions("female, low pitch");
        player.setSpeechOptions(QJsonObject{{"steps", 16}, {"seed", 42}});
        player.speak("This is a complete sentence.");
        QTRY_COMPARE(server.requests.size(), 1);
        auto payload = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("instructions").toString(), QString("female, low pitch"));
        QCOMPARE(payload.value("options").toObject().value("seed").toInt(), 42);
        QVERIFY(!payload.contains("voice"));
        QVERIFY(!payload.contains("speed"));
        player.stop();
        server.reply(0, "cancelled");
        player.setModel("piper-ru");
        player.setSpeechParameters(QJsonObject{{"speed", true}, {"voice_cloning", false}});
        player.speak("This is a different sentence.");
        QTRY_COMPARE(server.requests.size(), 2);
        payload = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QVERIFY(!payload.contains("instructions"));
        QVERIFY(!payload.contains("options"));
        QVERIFY(!payload.contains("voice"));
        QCOMPARE(payload.value("speed").toDouble(), 1.5);
        player.stop();
        server.reply(1, "cancelled");
    }

    void disablingAutoTranslationIgnoresThePendingResponse() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        MainWindow window(app_);
        app_->dictation()->finalTranscript("A complete sentence.", 1);
        window.autoTranslateBox_->setChecked(true);
        QTRY_COMPARE(server.requests.size(), 1);
        window.autoTranslateBox_->setChecked(false);
        QCOMPARE(window.resultEdit_->toPlainText(), QString("A complete sentence."));
        server.reply(0, R"({"result":"Late translation"})");
        QTest::qWait(40);
        QCOMPARE(window.resultEdit_->toPlainText(), QString("A complete sentence."));
        Settings restored;
        restored.load();
        QVERIFY(!restored.autoTranslate);
    }

    void speechInputUsesTheVoiceForItsLanguageAndCanStop() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        const QString previousRu = app_->settings()->ttsRussianVoice;
        const QString previousEn = app_->settings()->ttsEnglishVoice;
        app_->settings()->ttsRussianVoice = "my-russian-voice";
        app_->settings()->ttsEnglishVoice = "my-english-voice";
        VoicePlaybackDialog dialog(app_);
        auto *text = dialog.findChild<QPlainTextEdit *>("speechText");
        auto *play = dialog.findChild<QPushButton *>("speechToggle");
        QVERIFY(text && play);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"data":[]})");
        text->setPlainText(QString::fromUtf8("Привет, это мой голос."));
        play->click();
        QTRY_COMPARE(server.requests.size(), 2);
        auto payload = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("voice").toString(), QString("my-russian-voice"));
        QCOMPARE(payload.value("language").toString(), QString("ru"));
        QVERIFY(payload.value("input").toString().endsWith('.'));
        QCOMPARE(play->text(), QString("Stop"));
        play->click();
        QVERIFY(!app_->tts()->speaking());
        server.reply(1, "cancelled audio");
        QTest::qWait(30);
        text->setPlainText("Hello, this is my voice.");
        play->click();
        QTRY_COMPARE(server.requests.size(), 3);
        payload = QJsonDocument::fromJson(server.requests[2]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("voice").toString(), QString("my-english-voice"));
        QCOMPARE(payload.value("language").toString(), QString("en"));
        play->click();
        server.reply(2, "cancelled audio");
        QVERIFY(dialog.grab().save("voice-playback.png"));
        app_->settings()->ttsRussianVoice = previousRu;
        app_->settings()->ttsEnglishVoice = previousEn;
    }

    void shortPhrasesAreSpokenWithTheNextSentence() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        player.speak("Yes. This is a complete sentence.");
        QTRY_COMPARE(server.requests.size(), 1);   // the one-word phrase waits for the sentence
        const auto payload = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("input").toString(), QString("Yes. This is a complete sentence."));
        player.stop();
        server.reply(0, "cancelled");
    }

    void speechSynthesizesPhrasesAheadOfPlayback() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        player.speak("This is the first sentence. This is the second sentence. This is the third sentence.");
        QTRY_COMPARE(server.requests.size(), 2);   // two phrases ahead, not one at a time
        const auto first = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        const auto second = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(first.value("input").toString(), QString("This is the first sentence."));
        QCOMPARE(second.value("input").toString(), QString("This is the second sentence."));
        player.stop();
        server.reply(0, "cancelled");
    }

    void speechCanBeSavedToAWavFile() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        QSignalSpy saved(&player, &TtsPlayer::saved);
        QSignalSpy failed(&player, &TtsPlayer::saveFailed);
        QTemporaryDir directory(QDir::current().filePath("speech-save-XXXXXX"));
        const QString path = directory.filePath("out.wav");
        player.saveSpeech("This is a saved sentence. And this one is a second sentence.", path);
        QTRY_COMPARE(server.requests.size(), 1);
        QByteArray wav;
        QDataStream stream(&wav, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + 9600);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(1) << quint32(24000) << quint32(48000) << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << quint32(9600);
        for (int i = 0; i < 4800; ++i) stream << qint16(900 * std::sin(i * 6.283185307 * 220 / 24000));
        server.reply(0, wav);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 5000);   // the second sentence
        server.reply(1, wav);
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 5000);
        QCOMPARE(failed.size(), 0);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith("RIFF"));
        QCOMPARE(bytes.size(), 44 + 9600 * 2);          // both sentences, one header
        QCOMPARE(player.speaking(), false);             // saving must not play
    }

    void speechSilenceIsTrimmedAndPadded() {
        QByteArray pcm;
        const auto addSample = [&pcm](qint16 value) {
            pcm.append(reinterpret_cast<const char *>(&value), 2);
        };
        for (int i = 0; i < 400; ++i) addSample(0);
        for (int i = 0; i < 1200; ++i) addSample(3000);
        for (int i = 0; i < 400; ++i) addSample(0);
        QCOMPARE(trimSilence(pcm, 24000, 1, 0).size(), 1200 * 2);       // padding dropped
        QCOMPARE(trimSilence(pcm, 24000, 1, 120).size(), pcm.size());   // a 120 ms margin keeps all
        QCOMPARE(trimSilence(QByteArray(2000, '\0'), 24000, 1, 120).size(), 2000);  // never dropped
        QCOMPARE(appendSilence(QByteArray(100, '\0'), 24000, 1, 100).size(), 100 + 4800);
    }

    void speechPauseSettingIsPersistedAndApplied() {
        VoicePlaybackDialog dialog(app_);
        auto *pause = dialog.findChild<QSpinBox *>("speechPause");
        QVERIFY(pause);
        pause->setValue(250);
        QCOMPARE(app_->settings()->ttsPauseMs, 250);
        QCOMPARE(app_->tts()->pauseMs(), 250);
        Settings reloaded;
        reloaded.load();
        QCOMPARE(reloaded.ttsPauseMs, 250);
        pause->setValue(0);
        QCOMPARE(app_->settings()->ttsPauseMs, 0);
    }

    void voiceRecorderIncludesRussianAndEnglishReadingText() {
        SettingsDialog settings(app_);
        bool russian = false, english = false;
        QTimer guard;
        connect(&guard, &QTimer::timeout, this, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *language = dialog->findChild<QComboBox *>("referenceLanguage");
            auto *text = dialog->findChild<QPlainTextEdit *>("referenceTranscript");
            if (language && text) {
                russian = text->toPlainText().contains(QString::fromUtf8("Сегодня"));
                language->setCurrentIndex(language->findData("en"));
                english = text->toPlainText().contains("Today I am speaking");
                dialog->grab().save("voice-reference-recorder.png");
            }
            dialog->reject();
        });
        guard.start(50);
        QVERIFY(QMetaObject::invokeMethod(&settings, "recordVoice"));
        guard.stop();
        QVERIFY(russian && english);
    }

    void nativeAudioPlaybackStartsAndFinishes() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires an audio output device");
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        QSignalSpy started(&player, &TtsPlayer::started), finished(&player, &TtsPlayer::finished);
        player.speak("Audio output test.");
        QTRY_COMPARE(server.requests.size(), 1);
        QByteArray wav;
        QDataStream stream(&wav, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + 9600);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(1) << quint32(24000) << quint32(48000) << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << quint32(9600);
        for (int i = 0; i < 4800; ++i) stream << qint16(900 * std::sin(i * 6.283185307 * 220 / 24000));
        server.reply(0, wav);
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QVERIFY(!player.speaking());
    }

    void explicitSaveRecordsTheReviewedText() {
        QTemporaryDir directory(QDir::current().filePath("review-save-XXXXXX"));
        app_->store()->setDir(directory.path());
        const bool autoSave = app_->settings()->autoSaveSessions;
        app_->settings()->autoSaveSessions = false;
        MainWindow window(app_);
        app_->dictation()->finalTranscript("Original sentence.", 1);
        window.findChild<QTextEdit *>("resultText")->setPlainText("My reviewed sentence.");
        window.saveReviewedText();
        QDirIterator files(directory.path(), {"*.md"}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(files.hasNext());
        QFile record(files.next());
        QVERIFY(record.open(QIODevice::ReadOnly));
        const QString saved = QString::fromUtf8(record.readAll());
        QVERIFY(saved.contains("Original sentence."));
        QVERIFY(saved.contains("## Final (user-confirmed)\n\nMy reviewed sentence."));
        app_->settings()->autoSaveSessions = autoSave;
        app_->store()->setDir(app_->settings()->markdownDir);
    }

    void manualEditsSurviveLateResults() {
        MainWindow window(app_);
        QList<QTextEdit *> edits{window.findChild<QTextEdit *>("originalText"), window.findChild<QTextEdit *>("resultText")};
        app_->dictation()->finalTranscript("First sentence.", 1);
        edits[1]->setPlainText("My edited result");
        app_->dictation()->finalTranscript("Late sentence.", 1);
        QCOMPARE(edits[1]->toPlainText(), QString("My edited result"));
        QVERIFY(edits[0]->toPlainText().contains("Late sentence."));
    }

    void retryDoesNotDuplicateTheTranscript() {
        MainWindow window(app_);
        QList<QTextEdit *> edits{window.findChild<QTextEdit *>("originalText"), window.findChild<QTextEdit *>("resultText")};
        app_->dictation()->finalTranscript("One sentence.", 1);
        QVERIFY(QMetaObject::invokeMethod(&window, "retryProcessing"));
        QCOMPARE(edits[0]->toPlainText(), QString("One sentence."));
        QCOMPARE(edits[1]->toPlainText(), QString("One sentence."));
    }

    void compactBarCanBeDragged() {
        MainWindow window(app_);
        window.show();
        QCoreApplication::processEvents();
        auto *bar = window.findChild<QWidget *>("compactBar");
        QVERIFY(bar);
        const QPoint before = window.pos();
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QTest::mouseMove(bar, QPoint(44, 24));
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, QPoint(44, 24));
        QVERIFY(window.pos() != before);
    }

    void settingsRestoreTheVoiceModel() {
        app_->settings()->ttsModel = "profile";
        SettingsDialog dialog(app_);
        QComboBox *model = nullptr;
        for (auto *combo : dialog.findChildren<QComboBox *>())
            if (combo->findData("profile") >= 0) model = combo;
        QVERIFY(model);
        QCOMPARE(model->currentData().toString(), QString("profile"));
    }

    void snapToEveryEdge() {
        MainWindow window(app_);
        window.show();
        QCoreApplication::processEvents();
        const QRect area = window.screen()->availableGeometry().adjusted(8, 8, -8, -8);
        window.setDockPosition("top");
        QCOMPARE(window.y(), area.top());
        window.setDockPosition("bottom");
        QCOMPARE(window.geometry().bottom(), area.bottom());
        window.setDockPosition("left");
        QCOMPARE(window.x(), area.left());
        window.setDockPosition("right");
        QCOMPARE(window.geometry().right(), area.right());
        QCOMPARE(app_->settings()->dockPosition, QString("right"));
        window.setDockPosition("free");
    }

    void draggingGripDocksAtCursorEdge() {
        MainWindow window(app_);
        window.show();
        const QRect display = window.screen()->geometry();
        const QRect area = window.screen()->availableGeometry();
        const QPoint center = area.center();
        const QList<QPair<QString, QPoint>> drops{
            {"top", {center.x(), display.top() + 1}}, {"bottom", {center.x(), display.bottom() - 1}},
            {"left", {display.left() + 1, center.y()}}, {"right", {display.right() - 1, center.y()}},
            {"free", center + QPoint(-50, 100)}};
        auto *grip = window.placementButton_;
        for (const auto &[dock, drop] : drops) {
            window.setDockPosition("free");
            window.move(center - QPoint(window.width() / 2, 100));
            QCoreApplication::processEvents();
            const QPoint grab = grip->rect().center();
            const QPoint offset = grip->mapTo(&window, grab);
            QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, grab);
            QMouseEvent move(QEvent::MouseMove, grip->mapFromGlobal(drop), drop,
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(grip, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, grip->mapFromGlobal(drop), drop,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(grip, &release);
            QCOMPARE(app_->settings()->dockPosition, dock);
            if (dock == "free") QCOMPARE(window.pos(), drop - offset);
        }
    }

    void compactToolbarHasEqualInsetsAndOnlyIcons() {
        MainWindow window(app_);
        window.show();
        QSize horizontal;
        for (const QString &dock : {QString("free"), QString("top"), QString("bottom"), QString("left"), QString("right")}) {
            window.setDockPosition(dock);
            QCoreApplication::processEvents();
            QVERIFY(window.compactBar_->findChildren<QLabel *>().isEmpty());
            const auto buttons = window.compactBar_->findChildren<QPushButton *>();
            QCOMPARE(buttons.size(), 9);
            const bool vertical = dock == "left" || dock == "right";
            if (dock == "free") horizontal = window.size();
            QCOMPARE(window.size(), vertical ? horizontal.transposed() : horizontal);
            QRect bounds;
            for (int i = 0; i < buttons.size(); ++i) {
                auto *button = buttons[i];
                QVERIFY(button->text().isEmpty());
                QVERIFY(!button->icon().isNull());
                QCOMPARE(button->size(), QSize(36, 36));
                const QRect rect(button->mapTo(&window, QPoint()), button->size());
                if (i > 0) {
                    if (vertical) QCOMPARE(rect.top() - bounds.bottom() - 1, 8);
                    else QCOMPARE(rect.left() - bounds.right() - 1, 8);
                }
                bounds = bounds.united(rect);
            }
            QCOMPARE(bounds, window.rect().adjusted(10, 10, -10, -10));
        }
    }

    void previewKeepsToolbarAndButtonsInPlace() {
        MainWindow window(app_);
        window.show();
        for (const QString &dock : {QString("free"), QString("top"), QString("bottom"), QString("left"), QString("right")}) {
            window.showCompact();
            window.setDockPosition(dock);
            if (dock == "free") window.move(window.screen()->availableGeometry().center() - QPoint(320, 180));
            QCoreApplication::processEvents();
            const QRect toolbar = window.geometry();
            const QPoint microphone = window.micButton_->mapToGlobal(QPoint());
            window.showExpanded();
            QCoreApplication::processEvents();
            QCOMPARE(window.geometry(), toolbar);
            QCOMPARE(window.micButton_->mapToGlobal(QPoint()), microphone);
            const QRect preview = window.editorPage_->frameGeometry();
            QCOMPARE(window.editorPage_->grab().toImage().pixelColor(4, 100), QColor("#110e16"));
            QVERIFY(!preview.intersects(toolbar));
            if (dock == "free" || dock == "top" || dock == "bottom") {
                const QRect area = window.screen()->availableGeometry().adjusted(8, 8, -8, -8);
                QCOMPARE(preview.width(), qMin(720, area.width()));
                QCOMPARE(preview.x(), qBound(area.left(), toolbar.center().x() - window.editorPage_->rect().center().x(),
                                             area.right() - preview.width() + 1));
                if (dock == "bottom") QCOMPARE(preview.bottom() + 1, toolbar.top());
                else QCOMPARE(preview.top(), toolbar.bottom() + 1);
            }
            window.showCompact();
            QCOMPARE(window.geometry(), toolbar);
        }
    }

    void gripDoubleClickSnapsToEveryNearestEdge() {
        MainWindow window(app_);
        window.show();
        window.setDockPosition("free");
        QCoreApplication::processEvents();
        const QRect area = window.screen()->availableGeometry();
        const QSize panelSize = window.size();
        const int centeredX = area.center().x() - panelSize.width() / 2;
        const int centeredY = area.center().y() - panelSize.height() / 2;
        const QList<QPair<QString, QPoint>> locations{
            {"left", QPoint(area.left() + 15, centeredY)},
            {"right", QPoint(area.right() - panelSize.width() + 1 - 15, centeredY)},
            {"top", QPoint(centeredX, area.top() + 15)},
            {"bottom", QPoint(centeredX, area.bottom() - panelSize.height() + 1 - 15)}};
        for (const auto &[edge, point] : locations) {
            window.setDockPosition("free");
            window.move(point);
            auto *grip = window.findChild<QPushButton *>("placementButton");
            QVERIFY(grip);
            QTest::mouseDClick(grip, Qt::LeftButton);
            QCoreApplication::processEvents();
            QCOMPARE(app_->settings()->dockPosition, edge);
            if (edge == "left" || edge == "right") QCOMPARE(window.geometry().center().y(), area.center().y());
            else QCOMPARE(window.geometry().center().x(), area.center().x());
        }
    }

    void gripOwnsPlacementMenuAndRailIconsAreCentered() {
        MainWindow window(app_);
        window.setDockPosition("left");
        window.show();
        QCoreApplication::processEvents();
        auto *grip = window.findChild<QPushButton *>("placementButton");
        auto *menu = grip->findChild<QMenu *>("placementMenu");
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 5);
        const auto buttons = window.compactBar_->findChildren<QPushButton *>();
        QCOMPARE(buttons.size(), 9);   // placement, mic, preview, live, meeting, voice, subtitles, settings, close
        for (auto *control : buttons) {
            QCOMPARE(control->size(), QSize(36, 36));
            QCOMPARE(control->mapTo(&window, control->rect().center()).x(), window.rect().center().x());
        }
        QTest::mouseClick(grip, Qt::LeftButton);
        QCOMPARE(app_->settings()->dockPosition, QString("left"));
        QTRY_VERIFY(menu->isVisible());
        menu->hide();
        QVERIFY(window.grab().save("panel-vertical.png"));
        window.showExpanded();
        QVERIFY(capturePanel(window, "panel-vertical-preview.png"));
        window.hide();
        QVERIFY(!window.editorPage_->isVisible());
    }

    void editorSizeSurvivesCollapse() {
        MainWindow window(app_);
        window.showExpanded();
        window.editorPage_->resize(640, 570);
        const QSize expandedSize = window.editorPage_->size();
        window.showCompact();
        QCOMPARE(window.height(), 56);
        window.showExpanded();
        QCOMPARE(window.editorPage_->size(), expandedSize);
    }

    void settingsSaveRegistersBothHotkeys() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires Windows global hotkeys");
        app_->settings()->insertHotkey = "Ctrl+Alt+Shift+Y";
        app_->settings()->insertHotkey2 = "Ctrl+Alt+Shift+Z";
        app_->settings()->saveHotkey = "Ctrl+Alt+Shift+X";
        SettingsDialog dialog(app_);
        auto *save = button(dialog, "Save");
        QVERIFY(save);
        QString warning;
        QTimer modalGuard;
        connect(&modalGuard, &QTimer::timeout, this, [&warning]() {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warning = message->text();
                message->reject();
            }
        });
        modalGuard.start(100);
        save->click();
        modalGuard.stop();
        QVERIFY2(warning.isEmpty(), qPrintable(warning));
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        // The three dictation hotkeys are required; the two live-mode
        // hotkeys are optional because another application may own them.
        QVERIFY(app_->hotkeys()->contains(app_->settings()->insertHotkey));
        QVERIFY(app_->hotkeys()->contains(app_->settings()->insertHotkey2));
        QVERIFY(app_->hotkeys()->contains(app_->settings()->saveHotkey));
        QVERIFY(app_->hotkeys()->count() >= 3);
        app_->hotkeys()->unregisterAll();
    }

    void liveTranslationKeepsProcessingQueuedSpeechAfterAFailure() {
        for (const auto mode : {SessionController::Mode::Interpreter}) {
            TestServer server;
            ServiceClient client;
            client.setBaseUrl(server.url());
            TtsPlayer tts(&client);
            SessionController control(&client, &tts);
            control.configure(Settings{}, mode);
            control.running_ = true;
            QSignalSpy captions(&control, &SessionController::caption);
            QSignalSpy failures(&control, &SessionController::error);
            const QVector<float> samples(8000, 0.05f);
            for (int i = 0; i < 12; ++i) control.onUtterance(true, samples, double(i));
            for (int i = 0; i < 12; ++i) {
                QTRY_COMPARE(server.requests.size(), i * 2 + 1);
                server.reply(i * 2, QString("{\"text\":\"Phrase %1.\"}").arg(i).toUtf8());
                QTRY_COMPARE(server.requests.size(), i * 2 + 2);
                if (i == 4) server.reply(i * 2 + 1, R"({"detail":"temporary backend failure"})", false);
                else server.reply(i * 2 + 1, QString("{\"result\":\"Translation %1.\"}").arg(i).toUtf8());
                QTRY_VERIFY(!control.busy_ || server.requests.size() == i * 2 + 3);
                QVERIFY(control.running());
            }
            QTRY_COMPARE(captions.size(), 11);
            QCOMPARE(failures.size(), 1);
            QCOMPARE(captions.last()[1].toString(), QString("Phrase 11."));
            QVERIFY(control.remoteQueue_.isEmpty());
            control.stop();
        }
    }

    void transcriptOnlySubtitlesDoNotRequestTranslation() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        SessionController control(&client, nullptr);
        Settings settings;
        settings.subtitlesDisplay = 2;
        settings.meetingDiarize = false;        // the label has its own test
        control.configure(settings, SessionController::Mode::Subtitles);
        control.running_ = true;
        QSignalSpy captions(&control, &SessionController::caption);
        control.onUtterance(true, QVector<float>(8000, 0.05f), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 1000);
        server.reply(0, R"({"text":"first phrase"})");
        QTRY_COMPARE_WITH_TIMEOUT(captions.size(), 1, 1000);
        QCOMPARE(captions.first().at(1).toString(), QString("first phrase"));
        QVERIFY(captions.first().at(2).toString().isEmpty());
        QCOMPARE(server.requests.size(), 1);
    }

    void subtitleRecognitionContinuesWhileTranslationWaits() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        SessionController control(&client, nullptr);
        Settings settings;
        settings.meetingDiarize = false;        // the label has its own test
        control.configure(settings, SessionController::Mode::Subtitles);
        control.running_ = true;
        QSignalSpy captions(&control, &SessionController::caption);
        const QVector<float> samples(8000, 0.05f);
        control.onUtterance(true, samples, 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 1000);
        server.reply(0, R"({"text":"first"})");
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 1000);
        control.onUtterance(true, samples, 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 1000);
        server.reply(2, R"({"text":"second"})");
        server.reply(1, R"({"result":"temporary failure"})", false);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 4, 1000);
        server.reply(3, R"({"result":"second translated"})");
        QTRY_COMPARE_WITH_TIMEOUT(captions.size(), 1, 1000);
        QCOMPARE(captions.first().at(1).toString(), QString("second"));
        QCOMPARE(captions.first().at(2).toString(), QString("second translated"));
    }

    void subtitlesLabelSpeakersFromTheRollingWindow() {
        // Captions distinguish several voices on one stream exactly the way the
        // meeting does: a diarisation window overlapping the previous phrase, and
        // a known-turn match, so one person keeps one number across phrases.
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        SessionController control(&client, nullptr);
        Settings settings;
        settings.meetingDiarize = true;
        settings.subtitlesDisplay = 2;         // transcript only: no translator call
        control.configure(settings, SessionController::Mode::Subtitles);
        control.running_ = true;
        QSignalSpy captions(&control, &SessionController::caption);

        const QVector<float> samples(16000, 0.05f);          // one second
        for (int i = 0; i < 2; ++i) {
            const int before = server.requests.size();
            control.onUtterance(true, samples, 10.0 + i * 2.0);
            QTRY_COMPARE(server.requests.size(), before + 2);   // diariser and recogniser together
            const int diar = requestIndex(server, "diarize", before);
            const int asr = requestIndex(server, "transcriptions", before);
            QVERIFY(diar >= 0 && asr >= 0);
            const int from = int(i * 2.0 * 16000);              // phrase start on the window timeline
            const QString context = i
                ? QStringLiteral(R"({"start_sample":0,"end_sample":%1,"speaker_id":"speaker_0","confidence":0.97},)").arg(from)
                : QString();
            server.reply(diar, QStringLiteral(R"({"speaker_turns":[%1{"start_sample":%2,"end_sample":%3,"speaker_id":"speaker_0","confidence":0.99}]})")
                                    .arg(context).arg(from).arg(from + 16000).toUtf8());
            server.reply(asr, R"({"text":"hello there"})");
            QTRY_COMPARE(captions.size(), i + 1);
        }
        QCOMPARE(captions.at(0).at(0).toString(), QString("Speaker 1"));
        QCOMPARE(captions.at(1).at(0).toString(), QString("Speaker 1"));
        QCOMPARE(captions.at(1).at(1).toString(), QString("hello there"));
        QVERIFY(captions.at(1).at(2).toString().isEmpty());
        control.stop();
    }

    void subtitlesKeepThreeVoicesOnSeparateLabels() {
        // Three people take turns on one stream and the diariser renames its
        // ids in every request, so only the overlap with already-known speech
        // can keep a person on one line. The quiet voice used to be absorbed by
        // the most-recorded one: its spans were stored again in every window,
        // which made the overlap sum measure recording count, not identity.
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        SessionController control(&client, nullptr);
        Settings settings;
        settings.meetingDiarize = true;
        settings.subtitlesDisplay = 2;         // transcript only: no translator call
        control.configure(settings, SessionController::Mode::Subtitles);
        control.running_ = true;
        QSignalSpy captions(&control, &SessionController::caption);

        const QStringList ids{"speaker_0", "speaker_1", "speaker_2"};
        const QList<int> voices{0, 1, 0, 2, 1, 2};
        const QVector<float> samples(16000 * 5, 0.05f);   // five seconds each
        for (int i = 0; i < voices.size(); ++i) {
            const int before = server.requests.size();
            control.onUtterance(true, samples, 10.0 + i * 6.0);
            QTRY_COMPARE(server.requests.size(), before + 2);   // diariser and recogniser
            const int diar = requestIndex(server, "diarize", before);
            const int asr = requestIndex(server, "transcriptions", before);
            QVERIFY(diar >= 0 && asr >= 0);
            QJsonArray turns;
            for (int k = 0; k <= i; ++k) {
                // Same audio, different id names: identity may not follow them.
                const int id = (voices[k] + i) % 3;
                turns.append(QJsonObject{{"start_sample", k * 6 * 16000},
                                         {"end_sample", k * 6 * 16000 + samples.size()},
                                         {"speaker_id", ids[id]}});
            }
            server.reply(diar, QJsonDocument(QJsonObject{{"speaker_turns", turns}}).toJson());
            server.reply(asr, R"({"text":"hello there"})");
            QTRY_COMPARE(captions.size(), i + 1);
        }

        QCOMPARE(captions.at(0).at(0).toString(), QString("Speaker 1"));
        QCOMPARE(captions.at(1).at(0).toString(), QString("Speaker 2"));
        QCOMPARE(captions.at(2).at(0).toString(), QString("Speaker 1"));
        QCOMPARE(captions.at(3).at(0).toString(), QString("Speaker 3"));
        QCOMPARE(captions.at(4).at(0).toString(), QString("Speaker 2"));
        QCOMPARE(captions.at(5).at(0).toString(), QString("Speaker 3"));
        control.stop();
    }

    void interpreterTurnsIncomingSpeechIntoARussianCaption() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer tts(&client);
        SessionController control(&client, &tts);
        Settings settings;
        control.configure(settings, SessionController::Mode::Interpreter);
        control.running_ = true;
        QSignalSpy captions(&control, &SessionController::caption);

        QVector<float> samples(8000, 0.05f);
        control.onUtterance(true, samples, 2.0);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, "{\"text\":\"hello there\"}");
        QTRY_COMPARE(server.requests.size(), 2);
        const auto payload = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("mode").toString(), QString("translate_en_ru"));
        server.reply(1, "{\"result\":\"привет\"}");
        QTRY_COMPARE(captions.size(), 1);
        // The caption carries both columns: what was heard, what it became.
        QVERIFY(captions.first().at(0).toString().isEmpty());   // no speaker in the interpreter
        QCOMPARE(captions.first().at(1).toString(), QString("hello there"));
        QCOMPARE(captions.first().at(2).toString(), QString::fromUtf8("привет"));
        control.stop();
    }

    void liveAnswerSpeaksTheSavedCloneInsteadOfTheBuiltinVoice() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer tts(&client);
        SessionController control(&client, &tts);
        // Empty rows are what Settings::load leaves behind for a row the user
        // never chose; they used to reach the service as no voice at all, so the
        // call heard the built-in Piper voice instead of the saved clone.
        Settings settings;
        settings.ttsVoiceProfile = QStringLiteral("my-voice");
        control.configure(settings, SessionController::Mode::Interpreter);
        control.running_ = true;
        control.setOutgoing(SessionController::Outgoing::Translated);

        QVector<float> samples(8000, 0.05f);
        control.onUtterance(false, samples, 1.0);      // the microphone is the outgoing side
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"text":"privet"})");
        QTRY_COMPARE(server.requests.size(), 2);
        server.reply(1, R"({"result":"hello"})");
        QTRY_COMPARE(server.requests.size(), 3);
        const auto payload =
            QJsonDocument::fromJson(server.requests[2]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("voice").toString(), QStringLiteral("my-voice"));
        QCOMPARE(payload.value("model").toString(), QStringLiteral("auto"));
        tts.stop();
        server.reply(2, "cancelled");
        control.stop();
    }

    void retiredBuiltinVoiceRowMeansTheSavedVoice() {
        QCOMPARE(normalizeVoiceSelection(QStringLiteral("builtin")), QString());
        QCOMPARE(normalizeVoiceSelection(QString()), QString());
        QCOMPARE(normalizeVoiceSelection(QStringLiteral("my-voice")), QStringLiteral("my-voice"));
        QCOMPARE(SessionController::sessionVoice(Settings{}, QString()), QString());
        Settings saved;
        saved.ttsVoiceProfile = QStringLiteral("my-voice");
        QCOMPARE(SessionController::sessionVoice(saved, QString()), QStringLiteral("my-voice"));
        QCOMPARE(SessionController::sessionVoice(saved, QStringLiteral("other")), QStringLiteral("other"));
    }

    // Opt-in against a running service (VOICEPANEL_LIVE_TTS_URL, VOICEPANEL_LIVE_VOICE):
    // the answer the interpreter speaks must be synthesised by the saved profile,
    // not by the small built-in Russian voice.
    void liveInterpreterAnswerUsesTheSavedProfile() {
        const QString url = qEnvironmentVariable("VOICEPANEL_LIVE_TTS_URL");
        if (url.isEmpty()) QSKIP("Opt-in live TTS check");
        const QString profile = qEnvironmentVariable("VOICEPANEL_LIVE_VOICE");
        QVERIFY(!profile.isEmpty());

        ServiceClient client;
        client.setBaseUrl(url);
        TtsPlayer tts(&client);
        SessionController control(&client, &tts);
        Settings settings;
        settings.ttsModel = QStringLiteral("auto");
        settings.ttsVoiceProfile = profile;
        control.configure(settings, SessionController::Mode::Interpreter);

        const auto rateOf = [](const QByteArray &wav) {
            return int(quint8(wav[24])) | int(quint8(wav[25])) << 8
                | int(quint8(wav[26])) << 16 | int(quint8(wav[27])) << 24;
        };
        const auto synthesize = [&tts, &rateOf](const QString &suffix) {
            const QString path = QDir::temp().filePath(
                QStringLiteral("voicepanel-live-answer-%1.wav").arg(suffix));
            QFile::remove(path);
            QSignalSpy saved(&tts, &TtsPlayer::saved);
            QSignalSpy failed(&tts, &TtsPlayer::saveFailed);
            tts.saveSpeech(QString::fromUtf8("Привет, это проверка моего голоса."), path);
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 180000;
            while (saved.isEmpty() && failed.isEmpty()
                   && QDateTime::currentMSecsSinceEpoch() < deadline) {
                QTest::qWait(100);
            }
            QFile file(path);
            if (saved.isEmpty() || !file.open(QIODevice::ReadOnly)) return 0;
            return rateOf(file.readAll());
        };

        const int cloneRate = synthesize(QStringLiteral("profile"));
        QVERIFY2(cloneRate > 0, "the service returned no audio for the saved profile");
        // The built-in Russian voice is another engine with another rate, so an
        // equal rate would mean the profile was ignored.
        tts.setModel(QStringLiteral("piper-ru"));
        tts.setLanguageVoices(QString(), QString());
        const int builtinRate = synthesize(QStringLiteral("builtin"));
        QVERIFY2(builtinRate > 0, "the service returned no audio for piper-ru");
        QVERIFY2(cloneRate != builtinRate,
                 "the saved profile produced the built-in voice's audio format");
        qInfo("live answer: %s -> %d Hz, piper-ru -> %d Hz",
              qPrintable(profile), cloneRate, builtinRate);
    }

    void meetingLabelsTheActiveSourceAndKeepsTimestamps() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer tts(&client);
        SessionController control(&client, &tts);
        Settings settings;
        control.configure(settings, SessionController::Mode::Meeting);
        control.running_ = true;
        QSignalSpy lines(&control, &SessionController::line);

        QVector<float> samples(8000, 0.05f);
        control.onUtterance(false, samples, 65.0);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, "{\"text\":\"first point\"}");
        QTRY_COMPARE(lines.size(), 1);
        QCOMPARE(lines.first().at(0).toString(), QString("You"));
        QCOMPARE(lines.first().at(1).toString(), QString("first point"));
        QCOMPARE(lines.first().at(2).toDouble(), 65.0);
        QVERIFY(control.transcript().contains("[01:05] You: first point"));
        control.stop();
    }

    void meetingSpeakerSurvivesConsecutiveUtterances() {
        // Regression: the diarisation timeline read utterance times as
        // milliseconds while they are seconds, so every turn fell outside the
        // phrase and each remote line came back as "Others". One speaker must
        // keep one number across phrases, too.
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer tts(&client);
        SessionController control(&client, &tts);
        Settings settings;
        settings.meetingDiarize = true;
        control.configure(settings, SessionController::Mode::Meeting);
        control.running_ = true;
        QSignalSpy lines(&control, &SessionController::line);

        QVector<float> samples(16000, 0.05f);          // one second
        for (int i = 0; i < 2; ++i) {
            const int before = server.requests.size();
            control.onUtterance(true, samples, 10.0 + i * 2.0);
            // Speaker separation and recognition run in different engine
            // processes, so both requests are in flight at once.
            QTRY_COMPARE(server.requests.size(), before + 2);
            const int diar = requestIndex(server, "diarize", before);
            const int asr = requestIndex(server, "transcriptions", before);
            QVERIFY(diar >= 0 && asr >= 0);
            // The window carries the previous phrase as context, so a real run
            // reports the earlier speech in it as well; both turns come back
            // with the same engine id (samples relative to the window). A
            // window anchored on the phrase start instead of its end shifts
            // this out of range and yields "Others".
            const int from = int(i * 2.0 * 16000);                  // phrase start on the timeline
            const QString context = i
                ? QStringLiteral(R"({"start_sample":0,"end_sample":%1,"speaker_id":"speaker_0","confidence":0.97},)").arg(from)
                : QString();
            server.reply(diar, QStringLiteral(R"({"speaker_turns":[%1{"start_sample":%2,"end_sample":%3,"speaker_id":"speaker_0","confidence":0.99}]})")
                                    .arg(context).arg(from).arg(from + 16000).toUtf8());
            server.reply(asr, R"({"text":"привет"})");
            QTRY_COMPARE(server.requests.size(), before + 3);        // correction
            server.reply(before + 2, R"({"result":"Привет."})");
            QTRY_COMPARE(lines.size(), i + 1);
        }
        QCOMPARE(lines.at(0).at(0).toString(), QString("Speaker 1"));
        QCOMPARE(lines.at(1).at(0).toString(), QString("Speaker 1"));
        control.stop();
    }

    void captionOverlayStartsAndStopsWithoutAMicrophone() {
        // Subtitles mode captures one application only: no microphone, so the
        // overlay must come up even on a machine with no input device.
        CaptionOverlay overlay(app_);
        overlay.show();
        QCoreApplication::processEvents();
        QVERIFY(overlay.grab().save("caption-overlay.png"));
        QCOMPARE(overlay.isVisible(), true);
        overlay.end();
        overlay.hide();
    }

    void captionOverlayReadsTranscriptOnlyAtTheLargeSize() {
        // Transcript-only mode makes the caption the main content, so it is
        // drawn at the translation size. A later text-size change used to put
        // it back to the footnote size and clip it at the old line height.
        Settings *settings = app_->settings();
        const int display = settings->subtitlesDisplay;
        const int size = settings->subtitlesTextSize;
        settings->subtitlesDisplay = 2;
        settings->subtitlesTextSize = 1;               // M: 15/21 px
        {
            CaptionOverlay overlay(app_);
            auto *heard = overlay.findChild<QLabel *>(QStringLiteral("captionHeard"));
            auto *translated = overlay.findChild<QLabel *>(QStringLiteral("captionTranslated"));
            QVERIFY(heard && translated);
            QVERIFY(!heard->isHidden());
            QVERIFY(translated->isHidden());
            QCOMPARE(heard->font().pixelSize(), 21);
        }
        settings->subtitlesTextSize = 2;               // L: 19/27 px
        {
            CaptionOverlay overlay(app_);
            auto *heard = overlay.findChild<QLabel *>(QStringLiteral("captionHeard"));
            QVERIFY(heard);
            QCOMPARE(heard->font().pixelSize(), 27);
        }
        settings->subtitlesDisplay = display;
        settings->subtitlesTextSize = size;
    }

    void sessionWindowOpensInBothModes() {
        // Reproduces the crash report: opening either live mode. The
        // interpreter shows both halves; subtitles reuses the call pane alone.
        SessionWindow window(app_);
        QVERIFY(window.findChild<QTextEdit *>(QStringLiteral("liveRemote")));
        QVERIFY(window.findChild<QTextEdit *>(QStringLiteral("liveMine")));
        window.openMode(SessionWindow::Mode::Interpreter);
        QCOMPARE(window.isVisible(), true);
        QVERIFY(window.findChild<QTextEdit *>(QStringLiteral("liveMine"))->isVisible());
        window.openMode(SessionWindow::Mode::Subtitles);
        QVERIFY(window.findChild<QTextEdit *>(QStringLiteral("liveRemote"))->isVisible());
        QVERIFY(!window.findChild<QTextEdit *>(QStringLiteral("liveMine"))->isVisible());
        window.hide();
    }

    void liveAndMeetingWindowsHaveLevelBadgesNotBars() {
        // The mic and call levels are two animated badges in the top row (the
        // microphone fills, the speaker reacts to call audio); the old design
        // had bar meters that froze whenever loopback capture went quiet.
        SessionWindow live(app_);
        QCOMPARE(live.findChildren<LevelBadge *>(QStringLiteral("micLevelBadge")).size(), 1);
        QCOMPARE(live.findChildren<LevelBadge *>(QStringLiteral("remoteLevelBadge")).size(), 1);
        QVERIFY(live.findChildren<QProgressBar *>().isEmpty());
        MeetingWindow meeting(app_);
        QCOMPARE(meeting.findChildren<LevelBadge *>(QStringLiteral("micLevelBadge")).size(), 1);
        QCOMPARE(meeting.findChildren<LevelBadge *>(QStringLiteral("remoteLevelBadge")).size(), 1);
        QVERIFY(meeting.findChildren<QProgressBar *>().isEmpty());
        // One shared RMS mapping everywhere: the panel icon and the badges agree.
        QCOMPARE(AudioMeter::normalized(0.0), 0.0);
        QVERIFY(AudioMeter::normalized(0.2) > 0.5);
        QCOMPARE(AudioMeter::normalized(1.0), 1.0);
    }

    void settingsKeepARecogniserTheServiceHasNotReportedYet() {
        // The combo is rebuilt from /v1/models. A saved id that the service
        // has not listed (no service running, or an older build) used to be
        // dropped to "Service default" on the next save.
        app_->settings()->asrModel = "nemotron";
        SettingsDialog dialog(app_);
        QTest::qWait(250);                       // give a late /v1/models reply its chance
        auto *save = button(dialog, "Save");
        QVERIFY(save);
        QTimer modalGuard;
        connect(&modalGuard, &QTimer::timeout, this, []() {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                message->reject();
        });
        modalGuard.start(100);
        save->click();
        modalGuard.stop();
        QCOMPARE(app_->settings()->asrModel, QString("nemotron"));
    }

    void recognitionFinishesInOrderBeforeSessionEnds() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        DictationController controller(&client);
        controller.sessionOpen_ = true;
        controller.params_.speculative = false;
        QSignalSpy finals(&controller, &DictationController::finalTranscript);
        QSignalSpy ended(&controller, &DictationController::sessionEnded);
        for (int i = 0; i < 2; ++i) {
            controller.segment_.fill(0.1f, 4096);
            controller.inSpeech_ = true;
            controller.finishSegment(true);
        }
        controller.stop();
        QTRY_COMPARE(server.requests.size(), 2);
        QCOMPARE(ended.size(), 0);
        server.reply(1, R"({"text":"Second sentence."})");
        QTest::qWait(30);
        QCOMPARE(finals.size(), 0);
        server.reply(0, R"({"text":"First sentence."})");
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(finals.size(), 2);
        QCOMPARE(finals[0][0].toString(), QString("First sentence."));
        QCOMPARE(finals[1][0].toString(), QString("Second sentence."));
    }

    void stopFinalizesTheRemainingAudioExactlyOnce() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        DictationController controller(&client);
        controller.sessionOpen_ = controller.inSpeech_ = true;
        controller.params_.speculative = false;
        controller.speechFrames_ = controller.params_.minSpeechFrames;
        controller.segment_.fill(0.1f, controller.speechFrames_ * Vad::kFrameSamples);
        QSignalSpy finals(&controller, &DictationController::finalTranscript);
        QSignalSpy ended(&controller, &DictationController::sessionEnded);
        controller.stop();
        controller.stop();
        QVERIFY(!controller.start());
        QTRY_COMPARE(server.requests.size(), 1);
        QCOMPARE(ended.size(), 0);
        server.reply(0, R"({"text":"Remaining audio."})");
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(finals.size(), 1);
        QCOMPARE(finals[0][0].toString(), QString("Remaining audio."));
        controller.stop();
        QTest::qWait(30);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(ended.size(), 1);
    }

    void cancelledRecognitionDoesNotReachTheNextSession() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        DictationController controller(&client);
        controller.sessionOpen_ = true;
        controller.segment_.fill(0.1f, 4096);
        controller.finishSegment(true);
        QSignalSpy finals(&controller, &DictationController::finalTranscript);
        QTRY_COMPARE(server.requests.size(), 1);
        controller.cancel();
        server.reply(0, R"({"text":"Cancelled transcript."})");
        QTest::qWait(50);
        QCOMPARE(finals.size(), 0);
    }

    void failedProcessingKeepsQueuedWords() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TextProcessor processor(&client);
        processor.setMode("correct_en");
        QSignalSpy failed(&processor, &TextProcessor::failed);
        processor.acceptFinal("First sentence.");
        QTRY_COMPARE(server.requests.size(), 1);
        processor.acceptFinal("Second sentence.");
        server.reply(0, R"({"detail":"backend unavailable"})", false);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(processor.pendingOriginal(), QString("First sentence. Second sentence."));
    }

    void processingResumesOnNewSpeechAfterAnErrorWithoutRestarting() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TextProcessor processor(&client);
        processor.setMode("correct_en");
        QSignalSpy failures(&processor, &TextProcessor::processingFailed);
        QSignalSpy batches(&processor, &TextProcessor::processedBatch);
        processor.acceptFinal("First sentence.");
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"detail":"backend unavailable"})", false);
        QTRY_COMPARE(failures.size(), 1);
        processor.acceptFinal("Second sentence.");
        QTRY_COMPARE(server.requests.size(), 2);
        const auto request = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(request.value("text").toString(), QString("First sentence. Second sentence."));
        server.reply(1, R"({"result":"First sentence. Second sentence."})");
        QTRY_COMPARE(batches.size(), 1);
        QVERIFY(processor.pendingOriginal().isEmpty());
        processor.acceptFinal("Third sentence.");
        QTRY_COMPARE(server.requests.size(), 3);
        server.reply(2, R"({"result":"Third sentence."})");
        QTRY_COMPARE(batches.size(), 2);
    }

    void generationLimitKeepsTheOriginalOutOfProcessedText() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TextProcessor processor(&client);
        processor.setMode("auto_correct");
        processor.setAutoTranslate(true);
        QSignalSpy batches(&processor, &TextProcessor::processedBatch);
        QSignalSpy failures(&processor, &TextProcessor::processingFailed);
        QSignalSpy tail(&processor, &TextProcessor::fragmentRemaining);
        QSignalSpy finished(&processor, &TextProcessor::finished);
        const QString original = QString::fromUtf8("Прокачка персонажа должна зависеть от лута.");
        processor.acceptFinal(original);
        QTRY_COMPARE(server.requests.size(), 1);
        const auto request = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(request.value("mode").toString(), QString("translate_ru_en"));
        server.reply(0, QString(R"json({"detail":"text processing stopped at the generation limit (300 tokens)",)json"
                                R"json("reason":"generation_limit","max_tokens":300,"original":"%1"})json").arg(original).toUtf8(), false);
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures[0][0].toString(), original);
        QVERIFY(failures[0][1].toString().contains("output limit"));
        QCOMPARE(batches.size(), 0);                         // never reported as translated
        QCOMPARE(processor.pendingOriginal(), original);     // kept for retry, review and copy
        processor.finish();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(tail.size(), 0);                            // not handed over as a final tail either
        QCOMPARE(batches.size(), 0);
        QCOMPARE(processor.pendingOriginal(), original);
        processor.acceptFinal("Next sentence.");
        QTRY_COMPARE(server.requests.size(), 2);
        const auto retry = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(retry.value("text").toString(), original + " Next sentence.");
        server.reply(1, R"({"result":"A completed translation."})");
        QTRY_COMPARE(batches.size(), 1);
        QVERIFY(processor.pendingOriginal().isEmpty());
    }

    void processingModeFollowsTheSentenceLanguage() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TextProcessor processor(&client);
        processor.setMode("auto_correct");
        QSignalSpy batches(&processor, &TextProcessor::processedBatch);
        const auto body = [&server](int index) {
            return QJsonDocument::fromJson(server.requests[index]->property("body").toByteArray()).object();
        };
        const QString russian = QString::fromUtf8("нам нужно исправить эта ошибка.");
        processor.acceptFinal(russian);
        QTRY_COMPARE(server.requests.size(), 1);
        QCOMPARE(body(0).value("mode").toString(), QString("correct_ru"));
        server.reply(0, QString::fromUtf8(R"({"result":"Нам нужно исправить эту ошибку."})").toUtf8());
        QTRY_COMPARE(batches.size(), 1);
        QCOMPARE(batches[0][0].toString(), russian);
        QCOMPARE(batches[0][1].toString(), QString::fromUtf8("Нам нужно исправить эту ошибку."));
        processor.acceptFinal("this are a test.");
        QTRY_COMPARE(server.requests.size(), 2);
        QCOMPARE(body(1).value("mode").toString(), QString("correct_en"));
        server.reply(1, R"({"result":"This is a test."})");
        QTRY_COMPARE(batches.size(), 2);
        processor.setAutoTranslate(true);
        processor.acceptFinal("Open the file main.cpp.");
        QTRY_COMPARE(server.requests.size(), 3);
        QCOMPARE(body(2).value("mode").toString(), QString("translate_en_ru"));
        server.reply(2, QString::fromUtf8(R"({"result":"Открой файл main.cpp."})").toUtf8());
        QTRY_COMPARE(batches.size(), 3);
        QCOMPARE(batches[2][1].toString(), QString::fromUtf8("Открой файл main.cpp."));
        processor.acceptFinal(russian);
        QTRY_COMPARE(server.requests.size(), 4);
        QCOMPARE(body(3).value("mode").toString(), QString("translate_ru_en"));
        server.reply(3, R"({"result":"We need to fix this error."})");
        QTRY_COMPARE(batches.size(), 4);
    }

    void failedTranslationIsVisibleAndNotProcessed() {
        TestServer server;
        app_->service()->setBaseUrl(server.url());
        app_->settings()->autoTranslate = true;
        MainWindow window(app_);
        window.recording_ = window.sessionActive_ = true;
        window.reviewMode_ = false;
        const QString original = QString::fromUtf8("Прокачка персонажа должна зависеть от лута.");
        window.onFinalTranscript(original, 1, 0);
        QTRY_COMPARE(server.requests.size(), 1);
        server.reply(0, R"({"detail":"limit","reason":"generation_limit"})", false);
        QTRY_VERIFY(window.processingLabel_->text().contains("Translation pending"));
        QVERIFY(window.expanded_);
        QCOMPARE(window.originalSoFar_, original);
        QVERIFY(window.processedSoFar_.isEmpty());           // automatic insertion sends only processed text
        QVERIFY(window.resultEdit_->toPlainText().contains(original));   // still visible for review and copy
        QVERIFY(window.insertedText_.isEmpty());
        window.recording_ = window.sessionActive_ = false;
        app_->processor()->reset();
    }

    void incompleteSentenceStaysVerbatim() {
        ServiceClient client;
        TextProcessor processor(&client);
        processor.setMode("correct_en");
        QSignalSpy tail(&processor, &TextProcessor::fragmentRemaining);
        QSignalSpy finished(&processor, &TextProcessor::finished);
        processor.acceptFinal("Keep this fragment");
        processor.finish();
        QCOMPARE(tail.size(), 1);
        QCOMPARE(tail[0][0].toString(), QString("Keep this fragment"));
        QCOMPARE(finished.size(), 1);
    }

    void stoppedAudioIgnoresLateSynthesis() {
        TestServer server;
        ServiceClient client;
        client.setBaseUrl(server.url());
        TtsPlayer player(&client);
        QSignalSpy errors(&player, &TtsPlayer::error);
        player.speak("Cancelled speech.");
        QTRY_COMPARE(server.requests.size(), 1);
        player.stop();
        server.reply(0, "invalid audio must not be played");
        QTest::qWait(50);
        QVERIFY(!player.speaking());
        QCOMPARE(errors.size(), 0);
    }

    void vadStateSurvivesRepeatedReset() {
        Vad vad(QCoreApplication::applicationDirPath() + "/models/silero_vad.onnx");
        QVERIFY(vad.valid());
        float silence[Vad::kFrameSamples]{};
        for (int i = 0; i < 20; ++i) {
            vad.reset();
            QVERIFY(vad.probability(silence) < 0.5f);
        }
    }

    void shortNoiseDoesNotGrowAnUnboundedSegment() {
        ServiceClient client;
        DictationController controller(&client);
        controller.setVadModel(QCoreApplication::applicationDirPath() + "/models/silero_vad.onnx");
        QVERIFY(controller.vadReady());
        controller.params_.speculative = false;
        controller.params_.silenceFrames = 4;
        controller.params_.minSpeechFrames = 6;
        controller.inSpeech_ = true;
        controller.speechFrames_ = 1;
        controller.segment_.fill(0.01f, 512);
        controller.processBlock(QVector<float>(512 * 10, 0.0f));
        QVERIFY(!controller.inSpeech_);
        QVERIFY(controller.segment_.isEmpty());
        QCOMPARE(controller.segmentId_, 0);
    }

    void microphoneStartsAndStops() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires microphone hardware");
        if (WasapiCapture::listDevices(true).size() < 2) QSKIP("No microphone available");
        WasapiCapture capture;
        QSignalSpy started(&capture, &WasapiCapture::started);
        QSignalSpy frames(&capture, &WasapiCapture::framesAvailable);
        QSignalSpy errors(&capture, &WasapiCapture::error);
        QVERIFY(capture.start(WasapiCapture::Source::Microphone));
        QTRY_VERIFY_WITH_TIMEOUT(!started.isEmpty() || !errors.isEmpty(), 5000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors[0][0].toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
        capture.stop();
        QVERIFY(!capture.active());
        QVERIFY(capture.start(WasapiCapture::Source::Microphone));
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 2, 5000);
        capture.stop();
    }

    void insertionPreservesExistingTextAndDoesNotSubmit() {
        // Stealing the real desktop focus races with whatever the user is
        // doing; run this check explicitly (VOICEPANEL_TARGET_TEST) like the
        // other target-field tests.
        if (!qEnvironmentVariableIsSet("VOICEPANEL_TARGET_TEST"))
            QSKIP("Opt-in: set VOICEPANEL_TARGET_TEST to verify insertion on the live desktop");
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the Windows desktop");
        QLineEdit target("before after");
        target.setWindowTitle("VoicePanel insertion regression");
        target.resize(360, 40);
        target.show();
        target.raise();
        target.activateWindow();
        target.setFocus();
        QVERIFY(QTest::qWaitForWindowActive(&target, 3000));
        const HWND targetHandle = reinterpret_cast<HWND>(target.winId());
        if (!SetForegroundWindow(targetHandle))
            QSKIP("Windows refused foreground activation. Verify insertion with a user-focused target.");
        QTRY_COMPARE_WITH_TIMEOUT(GetForegroundWindow(), targetHandle, 3000);
        target.setCursorPosition(7);
        auto focus = captureCurrentFocus();
        if (focus.windowHandle != quintptr(target.winId())) {
            releaseFocusTarget(focus);
            QFAIL("Windows did not focus the test's own target field");
        }
        QSignalSpy submit(&target, &QLineEdit::returnPressed);
        const auto report = insertIntoTarget(focus, "inserted ");
        qInfo("Insertion route: %d, accessible field: %d", int(report.result), int(focus.isEditable));
        releaseFocusTarget(focus);
        QVERIFY2(report.result == InsertResult::Ok || report.result == InsertResult::ClipboardFallback,
                 qPrintable(report.detail));
        QTRY_COMPARE(target.text(), QString("before inserted after"));
        QCOMPARE(submit.size(), 0);
        QTest::qWait(1100);
        target.setCursorPosition(target.text().size());
        auto again = captureCurrentFocus();
        const auto appended = insertIntoTarget(again, "Next sentence.");
        releaseFocusTarget(again);
        QVERIFY(appended.result == InsertResult::Ok || appended.result == InsertResult::ClipboardFallback);
        QTRY_COMPARE(target.text(), QString("before inserted after Next sentence."));
        QTest::qWait(1100);
    }

    void settingsFitAtMinimumWidth() {
        SettingsDialog dialog(app_);
        dialog.resize(560, 640);
        dialog.show();
        QCoreApplication::processEvents();
        auto *scroll = dialog.findChild<QScrollArea *>();
        QVERIFY(scroll);
        QVERIFY(scroll->widget()->width() <= scroll->viewport()->width());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
        QVERIFY(dialog.grab().save("panel-settings-narrow.png"));
    }

    void markdownRecordCanBeWritten() {
        MarkdownStore store;
        store.setDir(QDir::current().filePath("test-records"));
        SessionRecord record;
        record.sessionId = "regression";
        record.original = QString::fromUtf8("Hello. Привет.");
        record.started = record.ended = QDateTime::currentDateTime();
        QString error;
        QVERIFY2(store.saveRecord(record, &error), qPrintable(error));
        const QString path = QDir(store.dir()).filePath(record.started.toString("yyyy-MM-dd") +
            "/session-" + record.started.toString("HHmmss") + "-regression.md");
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(saved.readAll()).contains(record.original));
    }

    void cancelledProcessingIgnoresLateResponses() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        ServiceClient client;
        client.setBaseUrl(QString("http://127.0.0.1:%1").arg(server.serverPort()));
        TextProcessor processor(&client);
        processor.setMode("correct_en");
        QSignalSpy batches(&processor, &TextProcessor::processedBatch);
        processor.acceptFinal("Old sentence.");
        QTRY_VERIFY(server.hasPendingConnections());
        auto *socket = server.nextPendingConnection();
        QTRY_VERIFY(socket->bytesAvailable() > 0);
        socket->readAll();
        processor.reset();
        const QByteArray body = R"({"result":"Old corrected sentence."})";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
        QTest::qWait(80);
        QCOMPARE(batches.size(), 0);
    }

    // Long meeting acceptance: latency and memory must stay flat and the
    // speaker numbers must survive phrase after phrase. The fixture is ~6
    // minutes of a three-person AMI recording looped with pauses; build it with
    // runtime/bench/_mkmeeting.py and run against a live service:
    //   VOICEPANEL_MEETING_BENCH=http://127.0.0.1:8080
    void meetingLongRecordingKeepsSpeakersAndLatency() {
        const QString url = qEnvironmentVariable("VOICEPANEL_MEETING_BENCH");
        if (url.isEmpty()) QSKIP("Opt-in: set VOICEPANEL_MEETING_BENCH to the service URL for the long meeting run");
        const QVector<float> call = readWav16k(QStringLiteral("runtime/bench/meeting-long-16k.wav"));
        QVERIFY2(call.size() > 16000 * 60, "fixture missing: run runtime/bench/_mkmeeting.py");

        Settings settings = *app_->settings();
        settings.meetingDiarize = true;
        settings.meetingLanguage = 2;          // the fixture is English
        app_->service()->setBaseUrl(url);
        SessionController control(app_->service(), nullptr);
        control.configure(settings, SessionController::Mode::Meeting);
        control.running_ = true;

        QList<QPair<QString, QString>> lines;
        QStringList failures;
        connect(&control, &SessionController::line, this,
                [&lines](const QString &speaker, const QString &text, double) { lines.append(qMakePair(speaker, text)); });
        connect(&control, &SessionController::error, this,
                [&failures](const QString &message) { failures.append(message); });

        const auto phrases = splitPhrases(call);
        QVERIFY2(phrases.size() >= 20, qPrintable(QStringLiteral("only %1 phrases").arg(phrases.size())));

        QList<QPair<QString, qint64>> records;      // speaker label and end-to-end time
        for (const auto &[start, length] : phrases) {
            const int before = lines.size();
            QElapsedTimer clock;
            clock.start();
            control.onUtterance(true, call.mid(start, length), double(start) / 16000.0);
            QTRY_VERIFY_WITH_TIMEOUT(!control.busy_, 180000);
            records.append(qMakePair(lines.size() > before ? lines.last().first : QString(), clock.elapsed()));
        }
        control.stop();

        QSet<QString> speakers;
        QList<qint64> times;
        for (const auto &record : records) {
            if (!record.first.isEmpty()) speakers.insert(record.first);
            times.append(record.second);
        }
        auto median = [](QList<qint64> values) {
            std::sort(values.begin(), values.end());
            return values.value(values.size() / 2);
        };
        // The first third of a meeting is a ramp: the speaker-separation window
        // is still filling, so a phrase there is cheaper than the steady state.
        // The acceptance criterion is that the running meeting does not get
        // slower, so the middle third is the baseline.
        const int third = qMax(1, times.size() / 3);
        const qint64 firstThird = median(times.mid(0, third));
        const qint64 steady = median(times.mid(third, third));
        const qint64 lastThird = median(times.mid(times.size() - third));

        QDir().mkpath(QStringLiteral("runtime/reports/meeting-long"));
        QJsonArray perPhrase;
        for (const auto &record : records)
            perPhrase.append(QJsonObject{{"speaker", record.first}, {"ms", double(record.second)}});
        QJsonObject report{{"phrases", perPhrase},
                           {"median_first_third_ms", double(firstThird)},
                           {"median_steady_ms", double(steady)},
                           {"median_last_third_ms", double(lastThird)},
                           {"speakers", QJsonArray::fromStringList(speakers.values())},
                           {"errors", QJsonArray::fromStringList(failures)}};
        QFile out(QStringLiteral("runtime/reports/meeting-long/result.json"));
        if (out.open(QIODevice::WriteOnly))
            out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));

        qInfo("meeting: %lld phrases, %lld speakers, median ramp %lld -> steady %lld -> %lld ms",
              qint64(records.size()), qint64(speakers.size()), firstThird, steady, lastThird);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ").left(300)));
        QVERIFY2(speakers.size() >= 2 && speakers.size() <= 4,      // three voices, one spare number
                 qPrintable(speakers.values().join(", ")));
        QVERIFY2(lastThird <= steady * 1.3 + 500,
                 qPrintable(QStringLiteral("steady %1 ms -> %2 ms").arg(steady).arg(lastThird)));
    }

    // Long caption acceptance on ten minutes of a three-voice recording, at a
    // level the VAD hears end to end: the set of labels must stop growing early,
    // and per-phrase latency must not drift. Builds on the same rolling window
    // the meeting uses, so a voice keeps its number while the buffer trims
    // underneath it. Build the fixture from the 30-minute take (the source is
    // ~30 dB quieter, so only its loudest voice reaches the VAD):
    //   runtime/bench/meeting-3voices-16k.wav
    //   VOICEPANEL_SUBTITLES_BENCH=http://127.0.0.1:8080
    void subtitlesLongRecordingKeepsSpeakersAndLatency() {
        const QString url = qEnvironmentVariable("VOICEPANEL_SUBTITLES_BENCH");
        if (url.isEmpty()) QSKIP("Opt-in: set VOICEPANEL_SUBTITLES_BENCH to the service URL for the long caption run");
        const QVector<float> call = readWav16k(QStringLiteral("runtime/bench/meeting-3voices-16k.wav"));
        QVERIFY2(call.size() > 16000 * 300, "fixture missing: several minutes of 16 kHz audio");

        Settings settings = *app_->settings();
        settings.meetingDiarize = true;
        settings.subtitlesDisplay = 2;         // transcript only: labels and timing are the subject
        settings.subtitlesLanguage = 0;
        app_->service()->setBaseUrl(url);
        SessionController control(app_->service(), nullptr);
        control.configure(settings, SessionController::Mode::Subtitles);
        control.running_ = true;

        QList<QPair<QString, QString>> captions;
        QStringList failures;
        connect(&control, &SessionController::caption, this,
                [&captions](const QString &speaker, const QString &heard, const QString &) {
                    captions.append(qMakePair(speaker, heard));
                });
        connect(&control, &SessionController::error, this,
                [&failures](const QString &message) { failures.append(message); });

        const auto phrases = splitPhrases(call);
        QVERIFY2(phrases.size() >= 100, qPrintable(QStringLiteral("only %1 phrases").arg(phrases.size())));

        // Acceptance reference: one diarisation of the whole fixture, where the
        // engine hears all three voices at once. Every phrase is then scored
        // against the voice the reference says is speaking, so "three people on
        // three labels" is checked per voice instead of by label count alone.
        const QJsonArray reference = diarizeWhole(app_->service(), call);
        QVERIFY2(reference.size() >= 100, "reference diarisation of the fixture failed");
        struct RefTurn {
            double start;
            double end;
            QString id;
        };
        QList<RefTurn> referenceTurns;
        for (const QJsonValue &value : reference) {
            const QJsonObject turn = value.toObject();
            referenceTurns.append({turn.value(QStringLiteral("start_sample")).toDouble() / 16000.0,
                                   turn.value(QStringLiteral("end_sample")).toDouble() / 16000.0,
                                   turn.value(QStringLiteral("speaker_id")).toString()});
        }
        const auto referenceVoice = [&referenceTurns](double from, double to) {
            QMap<QString, double> overlap;
            for (const RefTurn &turn : referenceTurns) {
                const double hit = qMin(to, turn.end) - qMax(from, turn.start);
                if (hit > 0) overlap[turn.id] += hit;
            }
            QString best;
            double most = 0.0;
            for (auto it = overlap.constBegin(); it != overlap.constEnd(); ++it) {
                if (it.value() > most) {
                    most = it.value();
                    best = it.key();
                }
            }
            return best;
        };

        // Human transcript of the same recording: the acceptance scores the
        // words, not only the labels.
        const QVector<MetricTurn> human = loadReferenceTurns(
            QStringLiteral("runtime/bench/meeting-3voices-ref.json"));
        QVERIFY2(!human.isEmpty(),
                 "runtime/bench/meeting-3voices-ref.json missing: run "
                 "tools/bench/transcript_quality.py reference (see docs/testing.md)");
        const auto humanIn = [&human](double from, double to) {
            QVector<MetricTurn> out;
            for (const MetricTurn &turn : human)
                if (qMin(to, turn.end) - qMax(from, turn.start) > 0.05 && turn.start < to) out.append(turn);
            return out;
        };

        QStringList labels;                    // one entry per phrase, as the caption showed it
        QStringList truth;                     // reference voice of that phrase
        QStringList heardText;                 // words the caption carried, possibly empty
        QStringList humanTruth;                // person the human annotation says spoke
        QList<bool> emitted;                   // the caption appeared at all
        QList<qint64> times;
        QList<int> distinct;                   // how many labels the recording had produced so far
        QSet<QString> seen;
        for (const auto &[start, length] : phrases) {
            const int before = captions.size();
            QElapsedTimer clock;
            clock.start();
            control.onUtterance(true, call.mid(start, length), double(start) / 16000.0);
            QTRY_VERIFY_WITH_TIMEOUT(!control.busy_, 180000);
            const bool appeared = captions.size() > before;
            const QString label = appeared ? captions.last().first : QString();
            labels << label;
            heardText << (appeared ? captions.last().second : QString());
            truth << referenceVoice(double(start) / 16000.0, double(start + length) / 16000.0);
            QMap<QString, double> spoken;
            for (const MetricTurn &turn : humanIn(double(start) / 16000.0, double(start + length) / 16000.0))
                spoken[turn.speaker] += qMin(double(start + length) / 16000.0, turn.end)
                                      - qMax(double(start) / 16000.0, turn.start);
            QString person;
            double most = 0.0;
            for (auto it = spoken.constBegin(); it != spoken.constEnd(); ++it)
                if (it.value() > most) { most = it.value(); person = it.key(); }
            humanTruth << person;
            emitted << appeared;
            times << clock.elapsed();
            if (!label.isEmpty()) seen.insert(label);
            distinct << seen.size();
        }
        control.stop();

        auto median = [](QList<qint64> values) {
            std::sort(values.begin(), values.end());
            return values.value(values.size() / 2);
        };
        const int third = qMax(1, times.size() / 3);
        const qint64 steady = median(times.mid(third, third));
        const qint64 lastThird = median(times.mid(times.size() - third));
        // A label that first appears late is a renumbered voice, not a new
        // participant: the window forgot someone. All voices must be found by
        // the first third, and nothing may be added afterwards.
        const int foundEarly = distinct.value(third - 1);

        QDir().mkpath(QStringLiteral("runtime/reports/subtitles-long"));
        QJsonArray perPhrase, labelCounts;
        for (int i = 0; i < labels.size(); ++i) {
            perPhrase.append(QJsonObject{{"speaker", labels[i]}, {"ms", double(times[i])},
                                         {"start_s", double(phrases[i].first) / 16000.0},
                                         {"end_s", double(phrases[i].first + phrases[i].second) / 16000.0},
                                         {"text", heardText[i]}});
            labelCounts.append(distinct[i]);
        }

        // ---- quality of the final transcript -------------------------------
        // WER/CER compare the concatenated caption text with the concatenated
        // human transcript, so a phrase the VAD never cut still counts as lost
        // words. Lost turns are counted on the human timeline instead: a turn
        // with real words that no captioned phrase covers is a person the live
        // run never reported.
        QString liveAll, humanAll;
        for (const QString &text : heardText) if (!text.isEmpty()) liveAll += text + ' ';
        for (const MetricTurn &turn : human) humanAll += turn.text + ' ';
        const QList<QString> liveWords = metricTokens(liveAll);
        const QList<QString> humanWords = metricTokens(humanAll);
        const double werValue = humanWords.isEmpty()
            ? 0.0 : double(edits(humanWords, liveWords)) / humanWords.size();
        const QList<QChar> liveChars = metricChars(liveWords), humanChars = metricChars(humanWords);
        const double cerValue = humanChars.isEmpty()
            ? 0.0 : double(edits(humanChars, liveChars)) / humanChars.size();

        // Lost turns are counted by words, not by seconds: captions sit wall to
        // wall, so timing overlap alone always reads zero even when the model
        // returned one unrelated word for a turn. A turn is lost when fewer
        // than half of its words appear in the captions over its span.
        constexpr double lostTurnRecall = 0.5;
        int turnsChecked = 0, turnsLost = 0;
        int wordsChecked = 0, wordsMissed = 0, wordsInLostTurns = 0;
        double humanSeconds = 0.0, coveredSeconds = 0.0;
        for (const MetricTurn &turn : human) {
            const QList<QString> turnWords = metricTokens(turn.text);
            if (turnWords.size() < 3 || turn.end - turn.start < 0.8) continue;
            ++turnsChecked;
            wordsChecked += turnWords.size();
            humanSeconds += turn.end - turn.start;
            QHash<QString, int> heard;
            double covered = 0.0;
            for (int i = 0; i < phrases.size(); ++i) {
                if (heardText[i].isEmpty()) continue;
                const double from = double(phrases[i].first) / 16000.0;
                const double to = from + double(phrases[i].second) / 16000.0;
                const double hit = qMin(to, turn.end) - qMax(from, turn.start);
                if (hit <= 0) continue;
                covered += hit;
                for (const QString &word : metricTokens(heardText[i])) ++heard[word];
            }
            coveredSeconds += qMin(covered, turn.end - turn.start);
            QHash<QString, int> wanted;
            for (const QString &word : turnWords) ++wanted[word];
            int kept = 0;
            for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it)
                kept += qMin(it.value(), heard.value(it.key()));
            wordsMissed += turnWords.size() - kept;
            if (kept < lostTurnRecall * turnWords.size()) {
                ++turnsLost;
                wordsInLostTurns += turnWords.size();
            }
        }
        const double lostShare = turnsChecked ? double(turnsLost) / turnsChecked : 0.0;
        const double lostWordShare = wordsChecked > 0 ? double(wordsInLostTurns) / wordsChecked : 0.0;
        const double missingWordShare = wordsChecked > 0 ? double(wordsMissed) / wordsChecked : 0.0;
        const double coverage = humanSeconds > 0.0 ? coveredSeconds / humanSeconds : 0.0;
        // A phrase the recogniser returned no words for is a caption the room
        // never saw. Different from an uncovered turn: the audio reached the
        // model and the model said nothing.
        int captioned = 0;
        for (int i = 0; i < phrases.size(); ++i)
            if (emitted[i] && !heardText[i].isEmpty()) ++captioned;
        const int emptyCaptions = int(labels.size()) - captioned;
        const double emptyShare = labels.isEmpty() ? 0.0 : double(emptyCaptions) / labels.size();

        // Names: the best one-to-one mapping of live labels onto the people in
        // the human transcript, then the share of captioned phrases that label
        // called correctly. Two voices may not share a name.
        QMap<QString, QMap<QString, int>> labelPeople;
        for (int i = 0; i < phrases.size(); ++i) {
            if (!emitted[i] || heardText[i].isEmpty()) continue;
            if (!labels[i].isEmpty() && !humanTruth[i].isEmpty())
                labelPeople[labels[i]][humanTruth[i]] += 1;
        }
        QMap<QString, QString> nameMap;
        QSet<QString> namedPeople;
        for (int guard = 0; guard < labelPeople.size(); ++guard) {
            QString bestLabel, bestPerson;
            int most = 0;
            for (auto label = labelPeople.constBegin(); label != labelPeople.constEnd(); ++label) {
                if (nameMap.contains(label.key())) continue;
                for (auto person = label.value().constBegin(); person != label.value().constEnd(); ++person) {
                    if (namedPeople.contains(person.key()) || person.value() <= most) continue;
                    most = person.value();
                    bestLabel = label.key();
                    bestPerson = person.key();
                }
            }
            if (bestLabel.isEmpty()) break;
            nameMap[bestLabel] = bestPerson;
            namedPeople.insert(bestPerson);
        }
        int namedRight = 0;
        for (int i = 0; i < phrases.size(); ++i) {
            if (!emitted[i] || heardText[i].isEmpty() || labels[i].isEmpty()) continue;
            if (nameMap.value(labels[i]) == humanTruth[i]) ++namedRight;
        }
        const double nameAccuracy = captioned ? double(namedRight) / captioned : 0.0;

        // Terms: the user's glossary words the recording contains, plus the
        // recording's own frequent content words. Recall is the share of those
        // that the live transcript still says.
        static const QSet<QString> stopwords = [] {
            QSet<QString> set;
            for (const QString &word : QStringLiteral(
                     "about after again all also and any are because been before but can could did do does "
                     "doing for from had has have here how into its just like more much must not now off "
                     "one only other our out over said same she should some than that the their them then "
                     "there these they thing things this those too very was way were what when where which "
                     "while who why will with would yeah okay right well think know going want really "
                     "something little maybe still even back down first over two three").split(' '))
                set.insert(word);
            return set;
        }();
        QMap<QString, int> frequency;
        for (const QString &word : humanWords)
            if (word.size() >= 4 && !stopwords.contains(word)) frequency[word] += 1;
        QList<QPair<int, QString>> ranked;
        for (auto it = frequency.constBegin(); it != frequency.constEnd(); ++it)
            if (it.value() >= 3) ranked.append(qMakePair(it.value(), it.key()));
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        QStringList terms;
        for (const QString &term : Settings::termsFromContext(settings.context))
            if (humanAll.contains(term, Qt::CaseInsensitive) && !terms.contains(term, Qt::CaseInsensitive))
                terms << term;
        for (int i = 0; i < ranked.size() && terms.size() < 40; ++i)
            if (!terms.contains(ranked[i].second, Qt::CaseInsensitive)) terms << ranked[i].second;
        int termsKept = 0;
        for (const QString &term : terms) if (liveAll.contains(term, Qt::CaseInsensitive)) ++termsKept;
        const double termAccuracy = terms.isEmpty() ? 0.0 : double(termsKept) / terms.size();

        const QJsonObject quality{{"wer", werValue}, {"cer", cerValue},
                                  {"words", QJsonObject{{"reference", humanWords.size()},
                                                        {"transcript", liveWords.size()}}},
                                  {"lost_phrases", QJsonObject{{"checked", turnsChecked},
                                                               {"lost", turnsLost}, {"share", lostShare},
                                                               {"word_share", lostWordShare},
                                                               {"missing_word_share", missingWordShare},
                                                               {"recall_threshold", lostTurnRecall}}},
                                  {"speech_covered", coverage},
                                  {"empty_captions", QJsonObject{{"segments", qint64(labels.size())},
                                                                 {"empty", emptyCaptions},
                                                                 {"share", emptyShare}}},
                                  {"speaker_names", QJsonObject{{"accuracy", nameAccuracy},
                                                                 {"captioned", captioned}}},
                                  {"terms", QJsonObject{{"checked", terms.size()},
                                                        {"kept", termsKept}, {"accuracy", termAccuracy}}}};
        // Per-voice acceptance: every reference voice must sit on one label,
        // two voices may not share a label, and a label that appears late is a
        // renumbered voice. Phrase recognition can come back empty; those
        // phrases carry no caption and are reported, not counted with the ones
        // that were captioned without a speaker.
        QMap<QString, QMap<QString, int>> perVoice;   // reference voice -> label -> phrases
        int unlabeled = 0;
        int emittedCount = 0;
        for (int i = 0; i < phrases.size(); ++i) {
            if (!emitted[i]) continue;
            ++emittedCount;
            if (labels[i].isEmpty()) {
                ++unlabeled;
                continue;
            }
            perVoice[truth[i]][labels[i]] += 1;
        }
        // The report is written before any assertion, so a failed acceptance
        // run still leaves the per-voice numbers that explain it.
        struct VoiceScore {
            QString voice;
            QString label;
            int best = 0;
            int total = 0;
        };
        QList<VoiceScore> scores;
        QJsonObject voiceReport;
        QStringList voiceLines;
        for (auto voice = perVoice.constBegin(); voice != perVoice.constEnd(); ++voice) {
            VoiceScore score;
            score.voice = voice.key();
            QJsonObject counts;
            for (auto it = voice.value().constBegin(); it != voice.value().constEnd(); ++it) {
                score.total += it.value();
                counts.insert(it.key(), it.value());
                if (it.value() > score.best) {
                    score.best = it.value();
                    score.label = it.key();
                }
            }
            voiceReport.insert(voice.key(), counts);
            voiceLines << QStringLiteral("%1=%2 %3/%4")
                              .arg(score.voice, score.label).arg(score.best).arg(score.total);
            scores << score;
        }
        QStringList assigned;
        for (const VoiceScore &score : scores) {
            if (!score.label.isEmpty()) assigned << score.label;
        }

        QJsonObject report{{"phrases", perPhrase},
                           {"quality", quality},
                           {"labels", QJsonArray::fromStringList(QStringList(seen.values()))},
                           {"distinct_after_each_phrase", labelCounts},
                           {"voices", voiceReport},
                           {"unlabeled_captions", unlabeled},
                           {"median_steady_ms", double(steady)},
                           {"median_last_third_ms", double(lastThird)},
                           {"errors", QJsonArray::fromStringList(failures)}};
        QFile out(QStringLiteral("runtime/reports/subtitles-long/result.json"));
        if (out.open(QIODevice::WriteOnly))
            out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));

        qInfo("subtitles: %lld phrases, %lld labels, %d empty, %d without speaker, "
              "median steady %lld -> last %lld ms; %s",
              qint64(labels.size()), qint64(seen.size()), int(labels.size() - emittedCount),
              unlabeled, steady, lastThird, qPrintable(voiceLines.join(", ")));
        qInfo("quality: WER %.3f CER %.3f, speech covered %.1f%%, lost turns %.1f%% "
              "(%d of %d, %.1f%% of their words missing), empty captions %.1f%%, "
              "names %.1f%%, terms %.1f%% (%lld of %lld)",
              werValue, cerValue, coverage * 100.0, lostShare * 100.0, turnsLost, turnsChecked,
              missingWordShare * 100.0, emptyShare * 100.0,
              nameAccuracy * 100.0, termAccuracy * 100.0, qint64(termsKept), qint64(terms.size()));
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ").left(300)));
        for (const VoiceScore &score : scores) {
            QVERIFY2(score.total >= 5,
                     qPrintable(QStringLiteral("%1 has only %2 captioned phrases")
                                    .arg(score.voice).arg(score.total)));
            QVERIFY2(score.best * 5 >= score.total * 4,   // at least 80 % of its phrases
                     qPrintable(QStringLiteral("%1: best label %2 covers %3 of %4")
                                    .arg(score.voice, score.label)
                                    .arg(score.best).arg(score.total)));
        }
        QCOMPARE(assigned.size(), scores.size());        // every voice got a label
        QCOMPARE(QSet<QString>(assigned.begin(), assigned.end()).size(), scores.size());
        QCOMPARE(perVoice.size(), 3);                    // three voices in the fixture
        QCOMPARE(seen.size(), 3);                        // and three labels, no spare
        QVERIFY2(unlabeled * 20 <= emittedCount,
                 qPrintable(QStringLiteral("%1 of %2 captions had no speaker")
                                .arg(unlabeled).arg(emittedCount)));
        QVERIFY2(seen.size() == foundEarly,
                 qPrintable(QStringLiteral("%1 labels by the first third, %2 by the end")
                                .arg(foundEarly).arg(seen.size())));
        QVERIFY2(lastThird <= steady * 1.3 + 500,
                 qPrintable(QStringLiteral("steady %1 ms -> %2 ms").arg(steady).arg(lastThird)));
        // Transcript quality. The ceilings leave room for a different recording
        // and model, not for a regression: the live run must carry most of what
        // the room said on the right names.
        QVERIFY2(werValue <= 0.55,
                 qPrintable(QStringLiteral("WER against the human transcript is %1")
                                .arg(werValue, 0, 'f', 3)));
        QVERIFY2(coverage >= 0.6,
                 qPrintable(QStringLiteral("only %1 % of the room's speech reached a caption")
                                .arg(coverage * 100.0, 0, 'f', 1)));
        QVERIFY2(lostShare <= 0.3,
                 qPrintable(QStringLiteral("%1 of %2 human turns were lost")
                                .arg(turnsLost).arg(turnsChecked)));
        QVERIFY2(emptyShare <= 0.25,
                 qPrintable(QStringLiteral("%1 of %2 phrases came back without words")
                                .arg(emptyCaptions).arg(labels.size())));
        QVERIFY2(nameAccuracy >= 0.8,
                 qPrintable(QStringLiteral("labels named the right person on %1 % of captions")
                                .arg(nameAccuracy * 100.0, 0, 'f', 1)));
    }

    void captureWindows() {
        MainWindow window(app_);
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save("panel-compact.png"));
        window.showExpanded();
        app_->dictation()->finalTranscript(QString::fromUtf8("The transcript appears here. Здесь работает кириллица."), 2);
        QCoreApplication::processEvents();
        QVERIFY(capturePanel(window, "panel-editor.png"));
        window.editorPage_->resize(640, 400);
        QCoreApplication::processEvents();
        QVERIFY(capturePanel(window, "panel-editor-narrow.png"));
        SettingsDialog dialog(app_);
        dialog.resize(640, 700);
        dialog.show();
        QCoreApplication::processEvents();
        auto *scroll = dialog.findChild<QScrollArea *>();
        QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(dialog.grab().save("panel-settings.png"));
    }
};

int main(int argc, char **argv) {
    QTemporaryDir state(QDir::current().filePath("test-state-XXXXXX"));
    if (!state.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, state.path());
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "VoicePanel", "VoicePanel");
    settings.setValue("panel/markdownDir", state.filePath("sessions"));
    settings.setValue("panel/serviceUrl", "http://127.0.0.1:1");
    settings.setValue("panel/contextDocument", state.filePath("context.md"));
    settings.sync();
    PanelApp app(argc, argv);
    PanelTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "PanelTests.moc"
