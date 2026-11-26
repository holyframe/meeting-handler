#include "WindowsAudioLevels.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <QTimer>

using Microsoft::WRL::ComPtr;

namespace {
QString endpointId(IMMDevice* device) {
    LPWSTR rawId = nullptr;
    if (device) device->GetId(&rawId);
    const auto id = rawId ? QString::fromWCharArray(rawId) : QString{};
    ::CoTaskMemFree(rawId);
    return id;
}

QString endpointName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) return {};
    PROPVARIANT value{};
    QString name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
        name = QString::fromWCharArray(value.pwszVal);
    ::PropVariantClear(&value);
    return name;
}

bool isActiveMicrophone(const ComPtr<IMMDevice>& device) {
    DWORD state = 0;
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow flow{};
    return device && SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE &&
           SUCCEEDED(device.As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&flow)) && flow == eCapture;
}

struct Endpoint {
    QString id;
    ComPtr<IAudioEndpointVolume> volume;
    ComPtr<IAudioMeterInformation> meter;
    AudioLevels lastPublished;

    void bind(IMMDevice* device) {
        const auto nextId = endpointId(device);
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

    IMMDeviceEnumerator* deviceEnumerator() {
        if (!enumerator)
            ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(enumerator.GetAddressOf()));
        return enumerator.Get();
    }

    Endpoint& refresh(AudioEndpoint kind, const QString& microphoneId = {}) {
        ComPtr<IMMDevice> device;
        if (auto* source = deviceEnumerator()) {
            HRESULT result;
            if (kind == AudioEndpoint::Microphone && !microphoneId.isEmpty()) {
                result = source->GetDevice(reinterpret_cast<LPCWSTR>(microphoneId.utf16()), device.GetAddressOf());
                if (!isActiveMicrophone(device)) device.Reset();
            } else {
                const auto flow = kind == AudioEndpoint::Microphone ? eCapture : eRender;
                result = source->GetDefaultAudioEndpoint(flow, eConsole, device.GetAddressOf());
            }
            if (FAILED(result) && result != E_NOTFOUND) enumerator.Reset();
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

bool WindowsAudioLevels::ensureDevices() {
    if (!devices_) {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) return false;
        initializedCom_ = SUCCEEDED(result);
        devices_ = std::make_unique<Devices>();
    }
    return true;
}

void WindowsAudioLevels::listMicrophones() {
    QList<MicrophoneDevice> microphones;
    auto* enumerator = ensureDevices() ? devices_->deviceEnumerator() : nullptr;
    ComPtr<IMMDeviceCollection> inputs;
    UINT count = 0;
    if (enumerator && SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, inputs.GetAddressOf())) &&
        SUCCEEDED(inputs->GetCount(&count))) {
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            if (FAILED(inputs->Item(index, device.GetAddressOf()))) continue;
            const auto id = endpointId(device.Get());
            if (id.isEmpty()) continue;
            const auto name = endpointName(device.Get());
            microphones.append({id, name.isEmpty() ? id : name});
        }
    }
    emit microphonesListed(microphones);
}

void WindowsAudioLevels::selectMicrophone(const QString& deviceId) {
    microphoneDeviceId_ = deviceId;
    if (devices_) {
        devices_->refresh(AudioEndpoint::Microphone, microphoneDeviceId_);
        publish(AudioEndpoint::Microphone, true);
    }
}

void WindowsAudioLevels::toggleMicrophoneMute() {
    constexpr auto kind = AudioEndpoint::Microphone;
    if (ensureDevices()) {
        // A global shortcut can run while the toolbar's level polling is paused.
        const AudioLevels current = devices_->refresh(kind, microphoneDeviceId_).read();
        if (current.muted.has_value()) {
            setMuted(kind, current.deviceId, !*current.muted);
            return;
        }
        publish(kind, true);
    }
    emit writeFailed(kind);
}

void WindowsAudioLevels::setMonitoring(bool enabled) {
    if (!enabled) {
        timer_->stop();
        return;
    }
    if (!ensureDevices()) return;
    lastDeviceCheck_.invalidate();
    poll();
    timer_->start();
}

void WindowsAudioLevels::poll() {
    const bool checkDefaults = !lastDeviceCheck_.isValid() || lastDeviceCheck_.elapsed() >= 1000;
    for (auto kind : {AudioEndpoint::Microphone, AudioEndpoint::Speakers}) {
        if (checkDefaults) devices_->refresh(kind, microphoneDeviceId_);
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
    auto& endpoint = devices_->refresh(kind, microphoneDeviceId_);
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
    auto& speakers = devices_->refresh(kind);
    if (!speakers.volume || FAILED(increase ? speakers.volume->VolumeStepUp(nullptr)
                                            : speakers.volume->VolumeStepDown(nullptr)))
        emit writeFailed(kind);
    publish(kind, true);
}

void WindowsAudioLevels::setMuted(AudioEndpoint kind, const QString& deviceId, bool muted) {
    if (!devices_ || deviceId.isEmpty()) return;
    auto& endpoint = devices_->refresh(kind, microphoneDeviceId_);
    const bool applied = endpoint.id == deviceId && endpoint.volume &&
                         SUCCEEDED(endpoint.volume->SetMute(muted ? TRUE : FALSE, nullptr));
    publish(kind, true);
    if (!applied || endpoint.lastPublished.muted != muted) emit writeFailed(kind);
}
