#pragma once
#include "core/FocusTracker.h"
#include "core/MarkdownStore.h"
#include "core/TextDiff.h"
#include <QDateTime>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QTimer>

class QBoxLayout;
class QPushButton;
class QCheckBox;
class QTextEdit;
class QSplitter;
class PanelTests;

namespace VoicePanel {
class PanelApp;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(PanelApp *app, QWidget *parent = nullptr);
    ~MainWindow() override;
    void showCompact();
    void showExpanded();

public slots:
    void toggleDictation(bool reviewMode);
    void startDictation(bool reviewMode);
    void stopDictation();
    void cancelDictation();
    void openSettings();
    void setDockPosition(const QString &position);
    void saveReviewedText();
    void openVoicePlayback();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;

private slots:
    void onFinalTranscript(const QString &text, qreal seconds, int segmentId);
    void onProcessedBatch(const QString &original, const QString &result);
    void retryProcessing();

private:
    friend class ::PanelTests;
    void buildUi();
    void configureProcessing();
    void updateServiceStatus();
    bool saveSessionRecord(bool confirmed = false, bool force = false);
    SessionRecord sessionRecord(bool confirmed);
    void preserveTranscript();
    void restoreTranscript(int index);
    void updateOriginal();
    void updateResult();
    void updateActions();
    void finishSession();
    void startNewTranscript();
    void applyDockPosition();
    void updateDockLayout();
    void positionPreview();
    QString nearestEdge() const;
    void saveGeometrySettings();
    void reviewCorrection(bool accept);
    void reviewCorrections(bool accept);
    void showCorrections();
    QVector<TextChange> pendingChanges(const QString &original, const QString &result) const;
    void insertResult(bool automatic = false);
    void scheduleQuickInsert();
    void copyResult();
    void translateResult();
    void updateMicrophone(double rms = 0);

    PanelApp *app_;
    QBoxLayout *rootLayout_ = nullptr;
    QBoxLayout *barLayout_ = nullptr;
    QWidget *compactBar_ = nullptr;
    QWidget *editorPage_ = nullptr;
    QWidget *reviewBar_ = nullptr;
    QSplitter *comparison_ = nullptr;
    QPushButton *micButton_ = nullptr;
    QPushButton *placementButton_ = nullptr;
    QPushButton *expandButton_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QPushButton *insertButton_ = nullptr;
    QPushButton *copyButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *historyButton_ = nullptr;
    QPushButton *acceptButton_ = nullptr;
    QPushButton *rejectButton_ = nullptr;
    QPushButton *translateButton_ = nullptr;
    QCheckBox *autoTranslateBox_ = nullptr;
    QLabel *langLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *processingLabel_ = nullptr;
    QLabel *changeLabel_ = nullptr;
    QLabel *resultLabel_ = nullptr;
    QTextEdit *originalEdit_ = nullptr;
    QTextEdit *resultEdit_ = nullptr;
    QTimer statusTimer_;
    QTimer transcriptTimer_;
    QTimer insertTimer_;
    QTimer placementTimer_;

    bool expanded_ = false;
    bool recording_ = false;
    bool finishing_ = false;
    bool reviewMode_ = true;
    bool sessionActive_ = false;
    bool processingDone_ = false;
    bool insertionFailed_ = false;
    bool speechActive_ = false;
    bool insertedEarlierDocument_ = false;
    bool userEditedResult_ = false;
    bool reviewedConfirmed_ = false;
    bool changingGeometry_ = true;
    bool dragging_ = false;
    bool dragArmed_ = false;
    double microphoneLevel_ = 0;
    QPoint dragOffset_;
    QPoint dragStart_;
    FocusTarget target_{};
    QString sessionId_;
    QString originalSoFar_;
    QString processedSoFar_;
    QString insertedText_;
    QString finalFragment_;
    QString fragmentText_;
    QString translation_;
    QString translationLanguage_;
    QMap<int, QString> previews_;
    QVector<TextChange> corrections_;
    // Accepted corrections, remembered by text rather than by offset: later
    // batches move the offsets, and a decision must not be offered again.
    QVector<QPair<QString, QString>> acceptedChanges_;
    int currentCorrection_ = 0;
    quint64 generation_ = 0;
    QDateTime sessionStartedAt_;
    QDateTime sessionEndedAt_;
    struct PreservedTranscript {
        SessionRecord record;
        QString reviewedText;
        QString insertedText;
        FocusTarget target;
        QVector<QPair<QString, QString>> acceptedChanges;
        QVector<TextChange> corrections;
        bool insertionFailed = false;
        bool confirmed = false;
    };
    QVector<PreservedTranscript> preservedTranscripts_;
};
}
