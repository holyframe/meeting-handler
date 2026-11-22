#pragma once

#include <QTableWidget>

struct ManagedMeeting;

class MeetingAudioSummary final : public QTableWidget
{
    Q_OBJECT

public:
    explicit MeetingAudioSummary(QWidget* parent);
    void setMeetings(const QList<ManagedMeeting>& meetings);
    void updateMeeting(int row, const ManagedMeeting& meeting);

signals:
    void meetingNameDoubleClicked(const QString& key);

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
};
