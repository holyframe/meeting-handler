#pragma once

#include <QJsonObject>
#include <QString>

struct ManagedMeeting
{
    enum class Microphone { Unknown, Muted, Unmuted, AudioNotConnected };
    enum class Sound { Unknown, On, Off };

    QString windowHandle;
    qint64 processId = 0;
    QString tabRuntimeId;
    QString title;
    QString chromeTabName;
    QString host;
    QString meetingId;
    QString profileName;
    int windowNumber = 0;
    Microphone microphone = Microphone::Unknown;
    Sound sound = Sound::Unknown;

    QString key() const { return windowHandle + '/' + tabRuntimeId; }
    QString displayTitle() const { return title.isEmpty() ? QStringLiteral("Zoom meeting") : title; }
    QString automationName() const { return chromeTabName.isEmpty() ? title : chromeTabName; }
    quintptr handle() const
    {
        return static_cast<quintptr>(windowHandle.toULongLong(nullptr, 16));
    }

    static ManagedMeeting fromTab(const QJsonObject& tab);
    bool matches(const ManagedMeeting& other) const;
    bool hasKnownSound() const { return sound != Sound::Unknown; }
    QJsonObject automationTarget() const;
    void applyObservation(const QJsonObject& result);
};
