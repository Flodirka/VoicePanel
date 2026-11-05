#include "audio/WasapiCapture.h"

#include <QVector>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <processthreadsapi.h>
#include <QFileInfo>
#include <cmath>
#include <thread>

namespace {

// Process loopback (Windows 10 2004+): capture one application without a
// virtual audio device. The activation is asynchronous; this helper blocks the
// capture thread for at most five seconds.
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient *client = nullptr;

    ~ActivationHandler() {
        if (done) CloseHandle(done);
    }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *operation) override {
        HRESULT activated = E_FAIL;
        IUnknown *unknown = nullptr;
        result = operation->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(result) && SUCCEEDED(activated) && unknown) {
            unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client));
            unknown->Release();
        }
        SetEvent(done);
        return S_OK;
    }
    STDMETHOD(QueryInterface)(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
};

IAudioClient *activateProcessLoopback(quint32 pid, HRESULT *error) {
    if (error) *error = E_INVALIDARG;
    if (pid == 0) return nullptr;
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT prop{};
    PropVariantInit(&prop);
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

    auto *handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    const HRESULT started = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &prop, handler, &operation);
    IAudioClient *client = nullptr;
    if (SUCCEEDED(started) && operation) {
        const DWORD waited = WaitForSingleObject(handler->done, 5000);
        client = handler->client;
        handler->client = nullptr;
        if (error) {
            *error = (waited == WAIT_OBJECT_0) ? handler->result : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        operation->Release();
    } else if (error) {
        *error = started;
    }
    handler->Release();
    return client;
}

} // namespace

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
    const bool processLoopback = source == Source::ProcessLoopback;
    const quint32 processId = processId_;
    const int targetRate = sampleRate_;

    impl_->thread = std::thread([this, deviceId, loopback, processLoopback, processId, targetRate]() {
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
            if (processLoopback) {
                // The render endpoint is used only for its mix format; the
                // audio itself comes from the application.
                if (FAILED(enu->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
                    fail(tr("WASAPI: no default output device to describe the format"));
                    break;
                }
                HRESULT activationError = S_OK;
                client = activateProcessLoopback(processId, &activationError);
                if (!client) {
                    // The hex code is what identifies the cause: 0x80070002 =
                    // the application is gone, 0x88890008 / 0x80004005 = the OS
                    // refused the capture, 0x80070005 = access denied.
                    fail(tr("Could not capture that application (0x%1). Pick it again from "
                            "the list while it is playing sound.")
                             .arg(quint32(activationError), 8, 16, QLatin1Char('0')));
                    break;
                }
            } else {
                const std::wstring wid = deviceId.toStdWString();
                HRESULT dhr = wid.empty()
                    ? (loopback ? enu->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
                                : enu->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))
                    : enu->GetDevice(wid.c_str(), &dev);
                if (FAILED(dhr)) { fail(tr("WASAPI: device not found (%1)").arg(deviceId.isEmpty() ? tr("default") : deviceId.left(48))); break; }
                if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) {
                    fail(tr("WASAPI: cannot activate device")); break;
                }
            }
            const AUDCLNT_SHAREMODE share = AUDCLNT_SHAREMODE_SHARED;
            const auto flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               ((loopback || processLoopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
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

bool WasapiCapture::processLoopbackSupported() {
    static const bool supported = [] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT error = S_OK;
        IAudioClient *client = activateProcessLoopback(DWORD(GetCurrentProcessId()), &error);
        const bool ok = client != nullptr;
        if (client) client->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        return ok;
    }();
    return supported;
}

QList<QPair<quint32, QString>> WasapiCapture::listAudioProcesses() {
    QList<QPair<quint32, QString>> out;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDeviceCollection *devices = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void **)&enumerator)) &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT deviceCount = 0;
        devices->GetCount(&deviceCount);
        // Sessions live per endpoint: an application playing into a headset does
        // not appear on the default speakers, which is why a running Discord was
        // missing from the list.
        for (UINT d = 0; d < deviceCount; ++d) {
            IMMDevice *device = nullptr;
            if (FAILED(devices->Item(d, &device)) || !device) continue;
            IAudioSessionManager2 *manager = nullptr;
            IAudioSessionEnumerator *sessions = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                           (void **)&manager)) &&
                SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
                int count = 0;
                sessions->GetCount(&count);
                for (int i = 0; i < count; ++i) {
                    IAudioSessionControl *control = nullptr;
                    if (FAILED(sessions->GetSession(i, &control)) || !control) continue;
                    IAudioSessionControl2 *control2 = nullptr;
                    if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                          (void **)&control2)) && control2) {
                        DWORD pid = 0;
                        control2->GetProcessId(&pid);
                        bool known = false;
                        for (const auto &entry : out) {
                            if (entry.first == quint32(pid)) known = true;
                        }
                        if (pid > 0 && !known) {
                            QString name = QObject::tr("Process %1").arg(pid);
                            wchar_t path[MAX_PATH];
                            DWORD size = MAX_PATH;
                            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                            if (process) {
                                if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                                    name = QFileInfo(QString::fromWCharArray(path, int(size)))
                                               .completeBaseName();
                                }
                                CloseHandle(process);
                            }
                            out.append({quint32(pid), name});
                        }
                        control2->Release();
                    }
                    control->Release();
                }
            }
            releaseAll(&sessions);
            releaseAll(&manager);
            device->Release();
        }
    }
    releaseAll(&devices);
    releaseAll(&enumerator);
    if (SUCCEEDED(com)) CoUninitialize();
    return out;
}
