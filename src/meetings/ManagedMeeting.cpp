#include "ManagedMeeting.h"

ManagedMeeting ManagedMeeting::fromTab(const QJsonObject& tab)
{
    ManagedMeeting meeting;
    meeting.windowHandle = tab.value("windowHandle").toString();
    meeting.processId = tab.value("processId").toInteger();
    meeting.tabRuntimeId = tab.value("tabRuntimeId").toString();
    meeting.chromeTabName = tab.value("tabName").toString();
    meeting.title = meeting.chromeTabName;
    meeting.host = tab.value("host").toString();
    meeting.meetingId = tab.value("meetingId").toString();
    meeting.profileName = tab.value("profileName").toString();
    return meeting;
}

bool ManagedMeeting::matches(const ManagedMeeting& other) const
{
    return key() == other.key() || (!meetingId.isEmpty() && meetingId == other.meetingId);
}

QJsonObject ManagedMeeting::automationTarget() const
{
    return {
        {"windowHandle", windowHandle}, {"tabRuntimeId", tabRuntimeId},
        {"tabName", automationName()}, {"requestKey", key()}
    };
}

void ManagedMeeting::applyObservation(const QJsonObject& result)
{
    const QString runtimeId = result.value("tabRuntimeId").toString();
    if (!runtimeId.isEmpty())
        tabRuntimeId = runtimeId;
    const QString chromeName = result.value("tabName").toString();
    if (!chromeName.isEmpty())
    {
        if (title.isEmpty() || title == chromeTabName)
            title = chromeName;
        chromeTabName = chromeName;
    }

    if (!result.value("ok").toBool()
        || (result.contains("verified") && !result.value("verified").toBool()))
    {
        microphone = Microphone::Unknown;
        sound = Sound::Unknown;
        return;
    }

    const QString state = result.value("state").toString();
    microphone = state == "muted" ? Microphone::Muted
        : state == "unmuted" ? Microphone::Unmuted
        : state == "audioNotConnected" ? Microphone::AudioNotConnected : Microphone::Unknown;

    if (result.contains("audioState"))
    {
        const QString audioState = result.value("audioState").toString();
        sound = audioState == "connected" ? Sound::On
            : audioState == "disconnected" ? Sound::Off : Sound::Unknown;
    }
    else
    {
        // A microphone action does not change a previously verified computer
        // connection. Never infer sound-on from a microphone label by itself.
        if (microphone == Microphone::AudioNotConnected)
            sound = Sound::Off;
        else if (microphone == Microphone::Unknown || sound != Sound::On)
            sound = Sound::Unknown;
    }
}
