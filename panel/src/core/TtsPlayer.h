#pragma once
#include "core/ServiceClient.h"
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

// Local TTS playback: fetches WAV from the service and plays it through the
// Windows waveOut API (winmm) - no Qt Multimedia dependency, tiny footprint.
//
// Follows the Pithagoras speech pipeline: one synthesis producer and one audio
// consumer, running independently in sentence order, with at most two phrases
// synthesized ahead of playback. The player therefore does not wait for the
// next sentence to render before the current one ends. `pauseMs` adds silence
// between sentences; 0 keeps only the pause the voice itself produces.
namespace VoicePanel {

// 16-bit PCM helpers (also used by the tests).
QByteArray trimSilence(const QByteArray &pcm, int rate, int channels, int marginMs);
QByteArray appendSilence(const QByteArray &pcm, int rate, int channels, int ms);

class TtsPlayer : public QObject {
    Q_OBJECT
public:
    explicit TtsPlayer(ServiceClient *service, QObject *parent = nullptr);
    ~TtsPlayer() override;

    void setModel(const QString &model) { model_ = model; }
    void setVoiceProfile(const QString &profileId) { voiceProfile_ = profileId; }
    void setLanguageVoices(const QString &russian, const QString &english) {
        russianVoice_ = russian; englishVoice_ = english; languageVoices_ = true;
    }
    void setSpeed(double speed) { speed_ = speed; }
    void setPauseMs(int ms) { pauseMs_ = qBound(0, ms, 2000); }
    int pauseMs() const { return pauseMs_; }

    bool speaking() const;
    void speak(const QString &text);      // split into sentences and start playing
    void stop();                          // cancel playback and clear the queue
    // Synthesise the whole text into one WAV file instead of playing it.
    void saveSpeech(const QString &text, const QString &path);

private slots:
    void onWaveDone();

signals:
    void started();
    void finished();
    void error(const QString &message);
    void saved(const QString &path);
    void saveFailed(const QString &message);

private:
    struct Chunk {
        QByteArray pcm;
        int rate = 0;
        int channels = 0;
    };
    struct PendingBuffer {
        void *waveHeader = nullptr;   // WAVEHDR*
        QByteArray pcm;
    };

    static QStringList splitSentences(const QString &text);
    bool hasWork() const;
    void requestMore();
    void pump();
    bool openDevice(int rate, int channels);
    bool deviceMatches(int rate, int channels) const;
    void writeBuffer(const QByteArray &pcm);
    void closeDevice();
    void settle();
    void fail(const QString &message);
    void saveNext();
    void writeSavedFile();

    ServiceClient *service_;
    QString model_;
    QString voiceProfile_;
    QString russianVoice_;
    QString englishVoice_;
    bool languageVoices_ = false;
    double speed_ = 1.0;
    int pauseMs_ = 0;

    QStringList queue_;               // sentences not requested yet
    QList<Chunk> ready_;              // synthesized, waiting for the device
    QMap<int, Chunk> ordered_;        // replies that arrived out of order
    QList<PendingBuffer> pending_;    // handed to waveOut, not finished yet
    int inflight_ = 0;                // synthesis requests in flight
    int nextRequest_ = 0;             // id of the next request to issue
    int nextChunk_ = 0;               // id of the next chunk to play
    bool session_ = false;            // an utterance is being played
    bool started_ = false;            // the first buffer reached the device
    quint64 generation_ = 0;

    QStringList saveQueue_;
    QString savePath_;
    QByteArray savePcm_;
    int saveRate_ = 0;
    int saveChannels_ = 0;

    int deviceRate_ = 0;
    int deviceChannels_ = 0;
    void *waveOut_ = nullptr;         // HWAVEOUT
};

} // namespace VoicePanel
