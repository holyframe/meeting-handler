#include "WindowsAudioLevels.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <QTimer>

using Microsoft::WRL::ComPtr;

namespace {
struct Endpoint {
    QString id;
    ComPtr<IAudioEndpointVolume> volume;
    ComPtr<IAudioMeterInformation> meter;
    AudioLevels lastPublished;

    void bind(IMMDevice* device) {
        LPWSTR rawId = nullptr;
        if (device) device->GetId(&rawId);
        const auto nextId = rawId ? QString::fromWCharArray(rawId) : QString{};
        ::CoTaskMemFree(rawId);
        if (nextId != id) {
            volume.Reset();
            meter.Reset();
            id = nextId;
        }
        if (id.isEmpty()) return;
        if (!volume) device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(volume.GetAddressOf()));
        if (!meter) device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(meter.GetAddressOf()));
    }

    AudioLevels read() {
        AudioLevels levels;
        levels.deviceId = id;
        float scalar = 0;
        if (volume) {
            if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar)))
                levels.volumePercent = qBound(0, qRound(scalar * 100), 100);
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMute(&muted))) levels.muted = muted != FALSE;
            if (!levels.volumePercent && !levels.muted.has_value()) volume.Reset();
        }
        if (meter) {
            if (SUCCEEDED(meter->GetPeakValue(&scalar)))
                levels.peakPercent = qBound(0, qRound(scalar * 100), 100);
            else
                meter.Reset();
        }
        return levels;
    }
};
}

struct WindowsAudioLevels::Devices {
    ComPtr<IMMDeviceEnumerator> enumerator;
    Endpoint microphone;
    Endpoint speakers;

    Endpoint& endpoint(AudioEndpoint kind) {
        return kind == AudioEndpoint::Microphone ? microphone : speakers;
    }

    Endpoint& refreshDefault(AudioEndpoint kind) {
        if (!enumerator)
            ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(enumerator.GetAddressOf()));
        ComPtr<IMMDevice> device;
        if (enumerator) {
            const auto flow = kind == AudioEndpoint::Microphone ? eCapture : eRender;
            const HRESULT result = enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.GetAddressOf());
            if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) enumerator.Reset();
        }
        auto& selected = endpoint(kind);
        selected.bind(device.Get());
        return selected;
    }
};

WindowsAudioLevels::WindowsAudioLevels() : timer_(new QTimer(this)) {
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &WindowsAudioLevels::poll);
}

WindowsAudioLevels::~WindowsAudioLevels() {
    devices_.reset();
    if (initializedCom_) ::CoUninitialize();
}

void WindowsAudioLevels::setMonitoring(bool enabled) {
    if (!enabled) {
        timer_->stop();
        return;
    }
    if (!devices_) {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) return;
        initializedCom_ = SUCCEEDED(result);
        devices_ = std::make_unique<Devices>();
    }
    lastDeviceCheck_.invalidate();
    poll();
    timer_->start();
}

void WindowsAudioLevels::poll() {
    const bool checkDefaults = !lastDeviceCheck_.isValid() || lastDeviceCheck_.elapsed() >= 1000;
    for (auto kind : {AudioEndpoint::Microphone, AudioEndpoint::Speakers}) {
        if (checkDefaults) devices_->refreshDefault(kind);
        publish(kind);
    }
    if (checkDefaults) lastDeviceCheck_.start();
}

void WindowsAudioLevels::publish(AudioEndpoint kind, bool force) {
    auto& endpoint = devices_->endpoint(kind);
    const AudioLevels levels = endpoint.read();
    if (force || levels != endpoint.lastPublished) {
        endpoint.lastPublished = levels;
        emit levelsChanged(kind, levels);
    }
}

void WindowsAudioLevels::setVolume(AudioEndpoint kind, const QString& deviceId, int percent) {
    if (!devices_ || deviceId.isEmpty()) return;
    // A headset can change between displaying the slider and releasing it.
    auto& endpoint = devices_->refreshDefault(kind);
    if (endpoint.volume && endpoint.id == deviceId &&
        FAILED(endpoint.volume->SetMasterVolumeLevelScalar(qBound(0, percent, 100) / 100.0f, nullptr)))
        emit writeFailed(kind);
    // Hardware can round or reject a write. Always return the actual setting.
    publish(kind, true);
}

void WindowsAudioLevels::stepSpeakerVolume(bool increase) {
    constexpr auto kind = AudioEndpoint::Speakers;
    if (!devices_) {
        emit writeFailed(kind);
        return;
    }
    auto& speakers = devices_->refreshDefault(kind);
    if (!speakers.volume || FAILED(increase ? speakers.volume->VolumeStepUp(nullptr)
                                            : speakers.volume->VolumeStepDown(nullptr)))
        emit writeFailed(kind);
    publish(kind, true);
}

void WindowsAudioLevels::setMuted(AudioEndpoint kind, const QString& deviceId, bool muted) {
    if (!devices_ || deviceId.isEmpty()) return;
    auto& endpoint = devices_->refreshDefault(kind);
    const bool applied = endpoint.id == deviceId && endpoint.volume &&
                         SUCCEEDED(endpoint.volume->SetMute(muted ? TRUE : FALSE, nullptr));
    publish(kind, true);
    if (!applied || endpoint.lastPublished.muted != muted) emit writeFailed(kind);
}
