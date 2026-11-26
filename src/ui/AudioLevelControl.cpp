#include "AudioLevelControl.h"
#include "AppTheme.h"

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
    setMaximumWidth(180);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    auto* row = new QHBoxLayout;
    column->addLayout(row);
    row->setSpacing(4);
    muteButton_ = app_theme::toolbarIconButton(this);
    muteButton_->setObjectName(prefix + "Mute");
    muteButton_->setCheckable(true);
    muteButton_->setFocusPolicy(Qt::StrongFocus);
    connect(muteButton_, &QToolButton::toggled, this, [this](bool muted) {
        // Keep the checked state at the last Windows readback.
        const QSignalBlocker guard(muteButton_);
        muteButton_->setChecked(levels_.muted.value_or(false));
        if (levels_.muted.has_value() && muted != *levels_.muted && !levels_.deviceId.isEmpty())
            emit muteRequested(endpoint_, levels_.deviceId, muted);
    });
    row->addWidget(muteButton_);

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
    deviceLabel_ = new QLabel(this);
    deviceLabel_->setObjectName(prefix + "DeviceName");
    deviceLabel_->setTextFormat(Qt::PlainText);
    deviceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    column->addWidget(deviceLabel_);
    updateDeviceLabel();

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

void AudioLevelControl::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateDeviceLabel();
}

void AudioLevelControl::updateDeviceLabel() {
    const QString name = levels_.deviceId.isEmpty() ? tr("Device unavailable")
        : levels_.deviceName.isEmpty() ? tr("Unnamed device") : levels_.deviceName;
    deviceLabel_->setText(deviceLabel_->fontMetrics().elidedText(name, Qt::ElideRight, contentsRect().width()));
    deviceLabel_->setToolTip(name);
    deviceLabel_->setAccessibleName(name);
}

void AudioLevelControl::syncSlider() {
    const QSignalBlocker guard(slider_);
    slider_->setValue(levels_.volumePercent.value_or(0));
    percent_->setText(levels_.volumePercent ? tr("%1%").arg(*levels_.volumePercent) : tr("—"));
}

void AudioLevelControl::syncMuteControl() {
    const bool muted = levels_.muted.value_or(false);
    const bool available = levels_.muted.has_value() && !levels_.deviceId.isEmpty();
    const QSignalBlocker guard(muteButton_);
    // Cancel a press if its device or action changed before release.
    muteButton_->setDown(false);
    muteButton_->setChecked(muted);
    muteButton_->setEnabled(available);
    QString action;
    if (endpoint_ == AudioEndpoint::Microphone) {
        muteButton_->setText(QChar(muted ? 0xEC54 : 0xE720));
        action = muted ? tr("Unmute Windows microphone") : tr("Mute Windows microphone");
    } else {
        muteButton_->setText(QChar(muted ? 0xE74F : 0xE767));
        action = muted ? tr("Unmute Windows sound") : tr("Mute Windows sound");
    }
    muteButton_->setAccessibleName(action);
    muteButton_->setToolTip(available ? action : tr("Windows mute control unavailable"));
}

void AudioLevelControl::setLevels(AudioEndpoint endpoint, const AudioLevels& levels) {
    if (endpoint != endpoint_) return;
    const bool deviceChanged = levels.deviceId != levels_.deviceId || levels.deviceName != levels_.deviceName;
    const bool muteChanged = levels.deviceId != levels_.deviceId || levels.muted != levels_.muted;
    levels_ = levels;
    if (deviceChanged) updateDeviceLabel();
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
