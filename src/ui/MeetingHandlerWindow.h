#pragma once

#include <QMainWindow>

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

private:
    void setColumnWidths(int sidebarWidth);
    QSplitter* columns_ = nullptr;
    MeetingControllerPane* controller_ = nullptr;
    int sidebarWidth_;
    int meetingColumnWidth_;
    bool restoreColumns_ = true;
    bool captionsReady_ = false;
};
