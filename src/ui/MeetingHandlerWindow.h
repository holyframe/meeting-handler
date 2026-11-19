#pragma once

#include <QMainWindow>

class QSettings;
struct Settings;

class MeetingHandlerWindow final : public QMainWindow {
public:
    MeetingHandlerWindow(const Settings& captionSettings, QSettings& layout);
    bool captionsReady() const { return captionsReady_; }

private:
    bool captionsReady_ = false;
};
