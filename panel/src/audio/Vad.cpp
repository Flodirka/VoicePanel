#include "audio/Vad.h"

#include <onnxruntime_cxx_api.h>
#include <cstring>

namespace VoicePanel {

struct Vad::State {
    ::Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "voicepanel-vad"};
    ::Ort::SessionOptions opts;
    std::unique_ptr<::Ort::Session> session;
    ::Ort::MemoryInfo mem = ::Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    ::Ort::Value state{nullptr};
    bool stateReady = false;
    std::array<float, 2 * 128> initialState{};
};

Vad::Vad(const QString &onnxPath) : ort_(std::make_unique<State>()) {
    ort_->opts.SetIntraOpNumThreads(1);
    ort_->opts.SetInterOpNumThreads(1);
    const std::wstring path = onnxPath.toStdWString();
    try {
        ort_->session = std::make_unique<::Ort::Session>(ort_->env, path.c_str(), ort_->opts);
        valid_ = true;
    } catch (const Ort::Exception &) {
        valid_ = false;
    }
    reset();
}

Vad::~Vad() = default;

void Vad::reset() {
    context_.fill(0.0f, 64);
    if (ort_->state)
        ort_->state = ::Ort::Value(nullptr);
    std::array<int64_t, 3> shape{2, 1, 128};
    ort_->initialState.fill(0.0f);
    ort_->state = ::Ort::Value::CreateTensor<float>(
        ort_->mem, ort_->initialState.data(), ort_->initialState.size(), shape.data(), shape.size());
    ort_->stateReady = true;
}

float Vad::probability(const float *frame512) {
    if (!valid_) return 0.0f;
    // input = context(64) + frame(512) = 576 samples, shape [1, 576]
    float input[64 + Vad::kFrameSamples];
    std::memcpy(input, context_.constData(), sizeof(float) * 64);
    std::memcpy(input + 64, frame512, sizeof(float) * kFrameSamples);
    const int64_t inShape[2] = {1, 64 + kFrameSamples};
    auto inTensor = ::Ort::Value::CreateTensor<float>(
        ort_->mem, input, 64 + kFrameSamples, inShape, 2);
    int64_t sr = 16000;
    const int64_t srShape[] = {1};
    auto srTensor = ::Ort::Value::CreateTensor<int64_t>(ort_->mem, &sr, 1, srShape, 1);
    const char *inNames[] = {"input", "state", "sr"};
    const char *outNames[] = {"output", "stateN"};
    // keep a raw copy: the returned state tensor must survive the next call
    Ort::Value inputs[] = {std::move(inTensor), std::move(ort_->state), std::move(srTensor)};
    auto outputs = ort_->session->Run(::Ort::RunOptions{nullptr}, inNames, inputs, 3, outNames, 2);
    float prob = 0.0f;
    if (outputs.size() >= 1 && outputs[0].IsTensor()) {
        prob = *outputs[0].GetTensorData<float>();
    }
    // new context = last 64 samples of what we fed
    context_ = QVector<float>(input + (64 + kFrameSamples) - 64, input + 64 + kFrameSamples);
    if (outputs.size() >= 2) {
        ort_->state = std::move(outputs[1]);
    } else {
        reset();
    }
    return prob;
}

} // namespace VoicePanel
