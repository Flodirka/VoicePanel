#include "audio/WasapiCapture.h"

#include <QVector>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cmath>
#include <thread>

#pragma comment(lib, "ole32.lib")

using namespace VoicePanel;

// Linear resampler: capture devices usually run 44.1/48 kHz; speech needs 16 kHz.
static QVector<float> resampleAudio(const float *in, size_t n, int srcRate, int targetRate, double &ratioState) {
    if (srcRate == targetRate) return {in, in + n};
    const double step = double(srcRate) / targetRate;
    QVector<float> out;
    out.reserve(int(n / step) + 2);
    double pos = ratioState;
    for (; pos < double(n); pos += step) {
        size_t i = size_t(pos);
        double frac = pos - double(i);
        float a = in[i < n ? i : n - 1];
        float b = in[(i + 1) < n ? i + 1 : n - 1];
        out.append(float(a + (b - a) * frac));
    }
    ratioState = pos - double(n);
    return out;
}

template <class T>
static void releaseAll(T **pp) {
    if (*pp) { (*pp)->Release(); *pp = nullptr; }
}

struct WasapiCapture::Impl {
    std::thread thread;
    HANDLE doneEvent = nullptr;
};

WasapiCapture::WasapiCapture(QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>()) {}

WasapiCapture::~WasapiCapture() { stop(); }

void WasapiCapture::setDevice(const QString &deviceId) { device_ = deviceId; }

QList<QPair<QString, QString>> WasapiCapture::listDevices(bool capture) {
    QList<QPair<QString, QString>> out{{"", QObject::tr("Default device")}};
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr);
    IMMDeviceEnumerator *enu = nullptr;
    IMMDeviceCollection *col = nullptr;
    if ((com || hr == RPC_E_CHANGED_MODE) && SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator), (void **)&enu)) &&
        SUCCEEDED(enu->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice *dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            LPWSTR id = nullptr;
            dev->GetId(&id);
            IPropertyStore *store = nullptr;
            QString name;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal) {
                    name = QString::fromWCharArray(pv.pwszVal);
                }
                PropVariantClear(&pv);
                store->Release();
            }
            out.append({QString::fromWCharArray(id), name});
            CoTaskMemFree(id);
            dev->Release();
        }
    }
    releaseAll(&col);
    releaseAll(&enu);
    if (com) CoUninitialize();
    return out;
}

bool WasapiCapture::start(Source source) {
    if (running_.load()) return true;
    stop();
    running_.store(true);
    impl_->doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    const QString deviceId = device_;
    const bool loopback = source == Source::Loopback;
    const int targetRate = sampleRate_;

    impl_->thread = std::thread([this, deviceId, loopback, targetRate]() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com = SUCCEEDED(hr);
        double resampleState = 0.0;

        IMMDeviceEnumerator *enu = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *client = nullptr;
        IAudioCaptureClient *capture = nullptr;
        WAVEFORMATEX *wfx = nullptr;
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        auto fail = [&](const QString &msg) {
            emit error(msg);
        };

        do {
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), (void **)&enu))) {
                fail(tr("WASAPI: cannot create enumerator")); break;
            }
            const std::wstring wid = deviceId.toStdWString();
            HRESULT dhr = wid.empty()
                ? (loopback ? enu->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
                            : enu->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))
                : enu->GetDevice(wid.c_str(), &dev);
            if (FAILED(dhr)) { fail(tr("WASAPI: device not found")); break; }
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) {
                fail(tr("WASAPI: cannot activate device")); break;
            }
            const AUDCLNT_SHAREMODE share = AUDCLNT_SHAREMODE_SHARED;
            const auto flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
            REFERENCE_TIME bufferDur = 0;  // default for shared mode + event
            if (FAILED(client->GetMixFormat(&wfx))) {
                fail(tr("WASAPI: cannot get format")); break;
            }
            if (FAILED(client->Initialize(share, flags, bufferDur, 0, wfx, nullptr))) {
                fail(tr("WASAPI: Initialize failed")); break;
            }
            if (FAILED(client->SetEventHandle(event))) {
                fail(tr("WASAPI: cannot set event")); break;
            }
            if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture))) {
                fail(tr("WASAPI: cannot get capture service")); break;
            }
            if (FAILED(client->Start())) { fail(tr("WASAPI: Start failed")); break; }
            emit started();

            QVector<float> pending;
            while (running_.load()) {
                if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
                BYTE *data = nullptr;
                UINT32 frames = 0;
                while (SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames > 0) {
                    DWORD bufFlags = 0;
                    if (FAILED(capture->GetBuffer(&data, &frames, &bufFlags, nullptr, nullptr))) break;
                    QVector<float> mono;
                    mono.reserve(int(frames));
                    if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        mono.fill(0.0f, int(frames));
                    } else {
                        for (UINT32 i = 0; i < frames; ++i) {
                            double acc = 0.0;
                            for (WORD ch = 0; ch < wfx->nChannels; ++ch) {
                                const size_t idx = size_t(i) * wfx->nChannels + ch;
                                double v = 0.0;
                                if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                    (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->wBitsPerSample == 32)) {
                                    v = ((const float *)data)[idx];
                                } else if (wfx->wBitsPerSample == 16) {
                                    v = double(((const int16_t *)data)[idx]) / 32768.0;
                                } else if (wfx->wBitsPerSample == 32) {
                                    v = double(((const int32_t *)data)[idx]) / 2147483648.0;
                                }
                                acc += v;
                            }
                            mono.append(float(acc / wfx->nChannels));
                        }
                    }
                    capture->ReleaseBuffer(frames);
                    const auto resampled = resampleAudio(mono.constData(), size_t(mono.size()),
                                                         int(wfx->nSamplesPerSec), targetRate, resampleState);
                    if (!resampled.isEmpty()) {
                        pending += resampled;
                        double sum = 0.0;
                        for (float s : resampled) sum += double(s) * double(s);
                        const double rms = std::sqrt(sum / resampled.size());
                        // hand over in ~1024-sample blocks (64 ms)
                        if (pending.size() >= 1024) {
                            QVector<float> block = pending.mid(0, pending.size() / 1024 * 1024);
                            pending.remove(0, block.size());
                            emit framesAvailable(block, rms);
                        }
                    }
                }
            }
            if (!pending.isEmpty()) {
                // Deliver the last short block before the controller finalizes
                // the recording. VAD consumes whole 512-sample frames.
                pending.resize((pending.size() + 511) / 512 * 512);
                emit framesAvailable(pending, 0);
            }
            client->Stop();
        } while (false);

        if (event) CloseHandle(event);
        if (wfx) CoTaskMemFree(wfx);
        releaseAll(&capture);
        releaseAll(&client);
        releaseAll(&dev);
        releaseAll(&enu);
        if (com) CoUninitialize();
        running_.store(false);
        emit stopped();
        SetEvent(impl_->doneEvent);
    });
    return true;
}

void WasapiCapture::stop() {
    running_.store(false);
    if (impl_->thread.joinable()) {
        if (impl_->doneEvent) WaitForSingleObject(impl_->doneEvent, 2000);
        impl_->thread.join();
    }
    if (impl_->doneEvent) { CloseHandle(impl_->doneEvent); impl_->doneEvent = nullptr; }
}
