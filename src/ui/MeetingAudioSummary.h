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
    void meetingNameActivated(const QString& key);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void activateMeetingName(QTableWidgetItem* cell);
};
