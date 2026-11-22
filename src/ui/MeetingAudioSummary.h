#pragma once

#include <QTableWidget>

struct ManagedMeeting;

class MeetingAudioSummary final : public QTableWidget
{
public:
    explicit MeetingAudioSummary(QWidget* parent);
    void setMeetings(const QList<ManagedMeeting>& meetings);
    void updateMeeting(int row, const ManagedMeeting& meeting);
};
