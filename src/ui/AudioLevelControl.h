#pragma once

#include "windows/WindowsAudioLevels.h"
#include <QWidget>

class QLabel;
class QProgressBar;
class QSlider;
class QToolButton;

class AudioLevelControl final : public QWidget {
    Q_OBJECT
public:
    explicit AudioLevelControl(AudioEndpoint endpoint, QWidget* parent);
    void setLevels(AudioEndpoint endpoint, const AudioLevels& levels);

signals:
    void volumeRequested(AudioEndpoint endpoint, const QString& deviceId, int percent);
    void muteRequested(AudioEndpoint endpoint, const QString& deviceId, bool muted);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateDeviceLabel();
    void syncSlider();
    void syncMuteControl();

    AudioEndpoint endpoint_;
    AudioLevels levels_;
    QString dragDeviceId_;
    QString description_;
    QToolButton* muteButton_;
    QLabel* percent_;
    QLabel* deviceLabel_;
    QSlider* slider_;
    QProgressBar* peak_;
};
