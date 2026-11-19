#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// The state of Zoom's computer-audio connection, not the Windows speaker volume.
namespace zoomAudio
{
enum class Connection { Unknown, Connected, Disconnected };
enum class Action { None, OpenMenu, JoinAudio, JoinComputerAudio, LeaveAudio, SelectComputerAudio };

struct Observation
{
    Connection connection = Connection::Unknown;
    QString controlName;
    QString microphoneState = "unknown";
    QString microphoneControlName;
    bool menuAvailable = false;
    bool joinAvailable = false;
    bool joinComputerAvailable = false;
    bool leaveAvailable = false;
    bool computerAudioTabAvailable = false;
    bool computerAudioTabSelected = false;
    bool computerAudioPanelVisible = false;

    bool chooserOpen() const
    {
        return joinComputerAvailable || computerAudioTabAvailable || computerAudioPanelVisible;
    }
};

Action actionForName(const QString& name);
Observation observe(const QStringList& visibleControlNames);
QString connectionName(Connection connection);

// The UIA adapter reacquires controls for every read and action. Tests supply
// changing observations without operating on a real meeting.
struct Backend
{
    std::function<Observation()> read;
    std::function<bool(Action)> invoke;
    std::function<void()> wait;
};

struct Result
{
    Observation observation;
    bool ok = false;
    bool changed = false;
    bool actionAttempted = false;
    QString error;
};

Result readConnection(const Backend& backend, int attempts = 15);
Result setConnected(const Backend& backend, bool connected, int attempts = 15);
}
