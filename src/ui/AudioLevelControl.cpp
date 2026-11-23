#include "AudioLevelControl.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

AudioLevelControl::AudioLevelControl(AudioEndpoint endpoint, QWidget* parent)
    : QWidget(parent), endpoint_(endpoint) {
    const bool mic = endpoint == AudioEndpoint::Microphone;
    const QString prefix = mic ? "microphone" : "speaker";
    description_ = mic ? tr("Windows microphone volume") : tr("Windows output volume");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    QWidget* icon;
    if (mic) {
        microphoneStatus_ = new QLabel(this);
        microphoneStatus_->setObjectName("microphoneStatus");
        microphoneStatus_->setAlignment(Qt::AlignCenter);
        microphoneStatus_->setTextInteractionFlags(Qt::NoTextInteraction);
        icon = microphoneStatus_;
    } else {
        speakerMute_ = new QToolButton(this);
        speakerMute_->setObjectName("speakerMute");
        speakerMute_->setCheckable(true);
        speakerMute_->setFocusPolicy(Qt::StrongFocus);
        speakerMute_->setCursor(Qt::PointingHandCursor);
        connect(speakerMute_, &QToolButton::toggled, this, [this](bool muted) {
            // Keep the checked state at the last Windows readback.
            const QSignalBlocker guard(speakerMute_);
            speakerMute_->setChecked(levels_.muted.value_or(false));
            if (levels_.muted.has_value() && muted != *levels_.muted && !levels_.deviceId.isEmpty())
                emit muteRequested(AudioEndpoint::Speakers, levels_.deviceId, muted);
        });
        icon = speakerMute_;
    }
    icon->setFixedSize(23, 23);
    icon->setFont(QFont("Segoe MDL2 Assets", 11));
    row->addWidget(icon);

    auto* tracks = new QVBoxLayout;
    tracks->setSpacing(1);
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setObjectName(prefix + "Volume");
    slider_->setAccessibleName(description_);
    slider_->setRange(0, 100);
    slider_->setTracking(false);
    slider_->setMinimumWidth(54);
    slider_->setFixedHeight(18);
    tracks->addWidget(slider_);
    peak_ = new QProgressBar(this);
    peak_->setObjectName(prefix + "Peak");
    peak_->setAccessibleName(mic ? tr("Windows microphone activity") : tr("Windows playback activity"));
    peak_->setRange(0, 100);
    peak_->setTextVisible(false);
    peak_->setFixedHeight(4);
    peak_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    tracks->addWidget(peak_);
    row->addLayout(tracks, 1);
    percent_ = new QLabel(this);
    percent_->setObjectName(prefix + "Percent");
    percent_->setMinimumWidth(fontMetrics().horizontalAdvance(tr("100%")));
    percent_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(percent_);

    connect(slider_, &QSlider::sliderPressed, this, [this] { dragDeviceId_ = levels_.deviceId; });
    connect(slider_, &QSlider::sliderMoved, this, [this](int value) {
        percent_->setText(tr("%1%").arg(value));
    });
    connect(slider_, &QSlider::sliderReleased, this, [this] {
        if (dragDeviceId_ != levels_.deviceId || slider_->sliderPosition() == slider_->value())
            syncSlider();
    });
    connect(slider_, &QSlider::valueChanged, this, [this](int percent) {
        if (!levels_.volumePercent || levels_.deviceId.isEmpty()) return;
        percent_->setText(tr("%1%").arg(percent));
        emit volumeRequested(endpoint_, levels_.deviceId, percent);
    });
    syncMuteControl();
    setLevels(endpoint_, {});
}

void AudioLevelControl::syncSlider() {
    const QSignalBlocker guard(slider_);
    slider_->setValue(levels_.volumePercent.value_or(0));
    percent_->setText(levels_.volumePercent ? tr("%1%").arg(*levels_.volumePercent) : tr("—"));
}

void AudioLevelControl::syncMuteControl() {
    const bool muted = levels_.muted.value_or(false);
    const bool available = levels_.muted.has_value() && !levels_.deviceId.isEmpty();
    if (microphoneStatus_) {
        QString status = muted ? tr("Windows microphone is muted") : tr("Windows microphone is on");
        if (!available) status = tr("Windows microphone status unavailable");
        microphoneStatus_->setText(QChar(muted ? 0xEC54 : 0xE720));
        microphoneStatus_->setAccessibleName(status);
        microphoneStatus_->setToolTip(status);
        microphoneStatus_->setEnabled(available);
        return;
    }
    const QString action = muted ? tr("Unmute Windows sound") : tr("Mute Windows sound");
    const QSignalBlocker guard(speakerMute_);
    // Cancel a press if its device or action changed before release.
    speakerMute_->setDown(false);
    speakerMute_->setChecked(muted);
    speakerMute_->setEnabled(available);
    speakerMute_->setText(QChar(muted ? 0xE74F : 0xE767));
    speakerMute_->setAccessibleName(action);
    speakerMute_->setToolTip(available ? action : tr("Windows mute control unavailable"));
}

void AudioLevelControl::setLevels(AudioEndpoint endpoint, const AudioLevels& levels) {
    if (endpoint != endpoint_) return;
    const bool muteChanged = levels.deviceId != levels_.deviceId || levels.muted != levels_.muted;
    levels_ = levels;
    if (muteChanged) syncMuteControl();
    const bool available = levels.volumePercent.has_value();
    slider_->setEnabled(available);
    slider_->setToolTip(available ? description_ : tr("%1 is unavailable").arg(description_));
    if (!slider_->isSliderDown() || !available) syncSlider();
    peak_->setEnabled(levels.peakPercent.has_value());
    peak_->setValue(levels.peakPercent.value_or(0));
    peak_->setToolTip(levels.peakPercent ? peak_->accessibleName()
                                         : tr("%1 is unavailable").arg(peak_->accessibleName()));
}
