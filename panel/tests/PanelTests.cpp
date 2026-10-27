#include "app/MainWindow.h"
#include "app/PanelApp.h"
#include "app/ServerSettingsDialog.h"
#include "app/SettingsDialog.h"
#include "app/VoicePlaybackDialog.h"
#include "app/ServiceTray.h"
#include "core/DictationController.h"
#include "core/MarkdownStore.h"
#include "core/ServiceClient.h"
#include "core/Settings.h"
#include "core/TextProcessor.h"
#include "core/TtsPlayer.h"
#include "core/HotkeyManager.h"
#include "audio/WasapiCapture.h"
#include <QScreen>
#include <QMessageBox>
#include <QMenu>
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
        window.setDockPosition("top");
        QCOMPARE(window.height(), 56);
        QVERIFY(window.width() >= 640);
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
        app_->service()->setBaseUrl("http://127.0.0.1:8080");
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
        text->setPlainText(QString::fromUtf8("Привет, это мой голос."));
        play->click();
        QTRY_COMPARE(server.requests.size(), 1);
        auto payload = QJsonDocument::fromJson(server.requests[0]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("voice").toString(), QString("my-russian-voice"));
        QCOMPARE(payload.value("language").toString(), QString("ru"));
        QVERIFY(payload.value("input").toString().endsWith('.'));
        QCOMPARE(play->text(), QString("Stop"));
        play->click();
        QVERIFY(!app_->tts()->speaking());
        server.reply(0, "cancelled audio");
        QTest::qWait(30);
        text->setPlainText("Hello, this is my voice.");
        play->click();
        QTRY_COMPARE(server.requests.size(), 2);
        payload = QJsonDocument::fromJson(server.requests[1]->property("body").toByteArray()).object();
        QCOMPARE(payload.value("voice").toString(), QString("my-english-voice"));
        QCOMPARE(payload.value("language").toString(), QString("en"));
        play->click();
        server.reply(1, "cancelled audio");
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
                QCOMPARE(preview.x(), toolbar.x());
                QCOMPARE(preview.width(), toolbar.width());
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
        const QRect area = window.screen()->availableGeometry();
        const QList<QPair<QString, QPoint>> locations{
            {"left", QPoint(area.left() + 15, area.center().y())},
            {"right", QPoint(area.right() - 655, area.center().y())},
            {"top", QPoint(area.center().x() - 320, area.top() + 15)},
            {"bottom", QPoint(area.center().x() - 320, area.bottom() - 70)}};
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
        QCOMPARE(buttons.size(), 5);
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
        QCOMPARE(app_->hotkeys()->count(), 3);
        app_->hotkeys()->unregisterAll();
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
