#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class QTimer;

enum class AudioEndpoint { Microphone, Speakers };
Q_DECLARE_METATYPE(AudioEndpoint)

struct AudioLevels {
    QString deviceId;
    std::optional<int> volumePercent;
    std::optional<int> peakPercent;
    std::optional<bool> muted;
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
    void setVolume(AudioEndpoint endpoint, const QString& deviceId, int percent);
    void stepSpeakerVolume(bool increase);
    void setMuted(AudioEndpoint endpoint, const QString& deviceId, bool muted);

signals:
    void levelsChanged(AudioEndpoint endpoint, const AudioLevels& levels);
    void writeFailed(AudioEndpoint endpoint);

private:
    struct Devices;
    void poll();
    void publish(AudioEndpoint endpoint, bool force = false);

    std::unique_ptr<Devices> devices_;
    bool initializedCom_ = false;
    QTimer* timer_;
    QElapsedTimer lastDeviceCheck_;
};
