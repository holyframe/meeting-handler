#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class QTimer;

enum class AudioEndpoint { Microphone, Speakers };
Q_DECLARE_METATYPE(AudioEndpoint)

struct MicrophoneDevice {
    QString id;
    QString name;
    bool operator==(const MicrophoneDevice&) const = default;
};
Q_DECLARE_METATYPE(MicrophoneDevice)

struct AudioLevels {
    QString deviceId;
    std::optional<int> volumePercent;
    std::optional<int> peakPercent;
    std::optional<bool> muted;
    QString deviceName;
    bool operator==(const AudioLevels&) const = default;
};
Q_DECLARE_METATYPE(AudioLevels)

// Lives on the audio thread. Only user requests write to Windows.
class WindowsAudioLevels final : public QObject {
    Q_OBJECT
public:
    WindowsAudioLevels();
    ~WindowsAudioLevels() override;
    void setMonitoring(bool enabled);
    void listMicrophones();
    void selectMicrophone(const QString& deviceId);
    void toggleMicrophoneMute();
    void setVolume(AudioEndpoint endpoint, const QString& deviceId, int percent);
    void stepSpeakerVolume(bool increase);
    void setMuted(AudioEndpoint endpoint, const QString& deviceId, bool muted);

signals:
    void microphonesListed(const QList<MicrophoneDevice>& microphones);
    void levelsChanged(AudioEndpoint endpoint, const AudioLevels& levels);
    void writeFailed(AudioEndpoint endpoint);

private:
    struct Devices;
    bool ensureDevices();
    void poll();
    void publish(AudioEndpoint endpoint, bool force = false);

    std::unique_ptr<Devices> devices_;
    QString microphoneDeviceId_;
    bool initializedCom_ = false;
    QTimer* timer_;
    QElapsedTimer lastDeviceCheck_;
};
