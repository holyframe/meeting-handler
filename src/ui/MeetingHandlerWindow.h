#pragma once

#include <QMainWindow>
#include <QList>

class QSettings;
class QSplitter;
class MeetingControllerPane;
struct Settings;

class MeetingHandlerWindow final : public QMainWindow {
public:
    MeetingHandlerWindow(const Settings& captionSettings, QSettings& layout);
    bool captionsReady() const { return captionsReady_; }

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void setColumnWidths(int sidebarWidth);
    void updateMinimumWidth();
    void applyMeetingsToggle();
    void restoreColumnWidths();
    int splitterHandlesWidth() const;
    int maximizedSidebarExtra() const;
    QSplitter* columns_ = nullptr;
    MeetingControllerPane* controller_ = nullptr;
    int sidebarWidth_;
    int meetingColumnWidth_;
    QList<int> widthsBeforeToggle_;
    QSize pendingNormalSize_;
    bool restoreColumns_ = true;
    bool captionsReady_ = false;
};
