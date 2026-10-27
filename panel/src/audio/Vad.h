#pragma once
#include <QString>
#include <QVector>
#include <memory>

// Silero VAD v5 via ONNX Runtime. Mirrors the official wrapper contract:
// each step feeds context(64) + frame(512) float samples at 16 kHz.
namespace VoicePanel {

class Vad {
public:
    static constexpr int kFrameSamples = 512;  // 32 ms at 16 kHz

    explicit Vad(const QString &onnxPath);
    ~Vad();
    bool valid() const { return valid_; }

    void reset();
    float probability(const float *frame512);  // one kFrameSamples frame

private:
    struct State;
    std::unique_ptr<State> ort_;
    QVector<float> context_;  // 64 samples carried between frames
    bool valid_ = false;
};

} // namespace VoicePanel
