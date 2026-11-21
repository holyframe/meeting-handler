#include "ZoomAudioState.h"

namespace
{
constexpr int kJoinButtonSettleReads = 2;

bool namesAction(const QString& text, const QString& action)
{
    return text == action || text.startsWith(action + " (");
}

bool isToolbarJoin(const QString& text)
{
    return namesAction(text, "join audio") || namesAction(text, "connect audio");
}

// Zoom's join button is <button>Join Audio by Computer<span class="loading">.
// The hidden span can be included in the accessible name, so a trailing word
// such as "loading" is still this button.
bool namesJoinComputer(const QString& text, const QString& action)
{
    return text == action
        || text.startsWith(action + " ")
        || text.startsWith(action + "(");
}

QString microphoneState(const QString& text)
{
    for (const QString& action : {QString("mute"), QString("unmute")})
    {
        if (namesAction(text, action)
            || text.startsWith(action + " my audio")
            || text.startsWith(action + " microphone")
            || text.startsWith(action + " my microphone"))
            return action == "mute" ? "unmuted" : "muted";
    }
    return {};
}
}

namespace zoomAudio
{
static bool canVerifyThroughMenu(const Observation& state)
{
    // Zoom can leave the tab/panel in the accessibility tree for a render
    // after a manual or automated join. Only the Leave item confirms audio.
    const bool microphoneReturned = state.microphoneState == "muted" || state.microphoneState == "unmuted";
    return state.menuAvailable && (!state.chooserOpen()
        || (microphoneReturned && !state.toolbarJoinVisible && !state.joinComputerAvailable));
}

Action actionForName(const QString& name)
{
    const QString text = name.simplified().toLower();
    if (text == "computer audio")
        return Action::SelectComputerAudio;
    for (const QString& action : {QString("join with computer audio"),
                                 QString("join computer audio"),
                                 QString("join audio by computer"),
                                 QString("connect to computer audio")})
    {
        if (namesJoinComputer(text, action))
            return Action::JoinComputerAudio;
    }
    for (const QString& action : {QString("leave computer audio"), QString("leave audio"),
                                 QString("disconnect computer audio"), QString("disconnect audio")})
    {
        if (namesAction(text, action))
            return Action::LeaveAudio;
    }
    if (isToolbarJoin(text))
        return Action::None;

    if ((text.contains("audio") || text.contains("mute"))
        && (text.contains("option") || text.contains("menu")
            || text.contains("controls") || text.contains("settings")
            || text.contains("more")))
        return Action::OpenMenu;

    return Action::None;
}

Observation observe(const QStringList& visibleControlNames)
{
    Observation result;
    QString connectedControl;
    QString disconnectedControl;
    QString observedMicrophone;
    bool microphoneConflict = false;

    for (const QString& name : visibleControlNames)
    {
        switch (actionForName(name))
        {
        case Action::JoinComputerAudio:
            result.joinComputerAvailable = true;
            disconnectedControl = name;
            break;
        case Action::LeaveAudio:
            result.leaveAvailable = true;
            connectedControl = name;
            break;
        case Action::OpenMenu:
            result.menuAvailable = true;
            break;
        case Action::SelectComputerAudio:
            result.computerAudioTabAvailable = true;
            break;
        default:
        {
            const QString text = name.simplified().toLower();
            if (isToolbarJoin(text))
            {
                result.toolbarJoinVisible = true;
                disconnectedControl = name;
                break;
            }
            const QString state = microphoneState(text);
            if (!state.isEmpty())
            {
                microphoneConflict = microphoneConflict
                    || (!observedMicrophone.isEmpty() && observedMicrophone != state);
                observedMicrophone = state;
                result.microphoneState = state;
                result.microphoneControlName = name;
            }
            break;
        }
        }
    }

    // Conflicting controls can occur during a render or in an unexpected Zoom
    // layout. Do not choose a state or perform a connection action in that case.
    if (!connectedControl.isEmpty() && disconnectedControl.isEmpty())
    {
        result.connection = Connection::Connected;
        result.controlName = connectedControl;
    }
    else if (connectedControl.isEmpty() && !disconnectedControl.isEmpty()
             && (observedMicrophone.isEmpty() || result.joinComputerAvailable))
    {
        result.connection = Connection::Disconnected;
        result.controlName = disconnectedControl;
        result.microphoneState = "audioNotConnected";
        result.microphoneControlName.clear();
    }
    else
    {
        if (!connectedControl.isEmpty() || !disconnectedControl.isEmpty())
            microphoneConflict = true;
    }
    if (microphoneConflict)
    {
        result.microphoneState = "unknown";
        result.microphoneControlName.clear();
    }
    return result;
}

QString connectionName(Connection connection)
{
    switch (connection)
    {
    case Connection::Connected: return "connected";
    case Connection::Disconnected: return "disconnected";
    default: return "unknown";
    }
}

static Result discoverConnection(const Backend& backend, int attempts, bool forSoundOn)
{
    Result result;
    bool menuOpened = false;
    bool tabSelected = false;
    bool waitingForChooser = false;
    for (int attempt = 0; ; ++attempt)
    {
        result.observation = backend.read();
        const auto& state = result.observation;
        if (state.connection != Connection::Unknown)
        {
            result.ok = true;
            return result;
        }
        if (attempt >= attempts)
            break;
        waitingForChooser = waitingForChooser || state.chooserOpen();
        if (!menuOpened && canVerifyThroughMenu(state))
        {
            if (!backend.invoke(Action::OpenMenu))
                break;
            menuOpened = true;
        }
        else if (!forSoundOn && !menuOpened && !tabSelected && state.computerAudioTabAvailable
                 && !state.computerAudioTabSelected && !state.computerAudioPanelVisible
                 && !state.joinComputerAvailable)
        {
            if (!backend.invoke(Action::SelectComputerAudio))
                break;
            tabSelected = true;
        }
        else if (!menuOpened)
        {
            // A newly activated tab can briefly expose no controls. Sound On
            // can re-read it, but a visible chooser is ready for the button check.
            const bool waitForControls = forSoundOn
                ? !waitingForChooser && attempt < kJoinButtonSettleReads
                : waitingForChooser;
            if (!waitForControls)
                break;
        }
        backend.wait();
    }
    result.error = forSoundOn ? "Zoom's computer-audio state could not be verified."
        : "Could not find the Join Audio or Leave Computer Audio button. The sound state was not changed.";
    return result;
}

Result readConnection(const Backend& backend, int attempts)
{
    return discoverConnection(backend, attempts, false);
}

static Result joinComputerAudio(const Backend& backend, int attempts)
{
    Result result = discoverConnection(backend, attempts, true);
    if (result.observation.connection == Connection::Connected)
        return result;
    if (!result.observation.joinComputerAvailable && !result.observation.leaveAvailable)
    {
        result.ok = false;
        result.error = "Could not find the Join Audio by Computer button in Zoom. "
                       "Open Zoom's audio chooser, select Computer Audio, and try again.";
        return result;
    }
    if (!result.ok)
        return result;

    bool verificationMenuOpened = false;
    result.ok = false;

    for (int attempt = 0; ; ++attempt)
    {
        const auto& state = result.observation;
        if (state.connection == Connection::Connected)
        {
            result.ok = true;
            result.changed = result.actionAttempted;
            return result;
        }
        if (attempt >= attempts)
            break;

        if (!result.actionAttempted)
        {
            result.actionAttempted = true;
            if (!backend.invoke(Action::JoinComputerAudio))
            {
                // The user may have joined between discovery and activation.
                Result current = discoverConnection(backend, attempts - attempt, true);
                if (current.ok && current.observation.connection == Connection::Connected)
                {
                    current.actionAttempted = true;
                    return current;
                }
                result.error = "The Join Audio by Computer button was found, but it could not be clicked. "
                               "The connection is unverified and the meeting was not joined.";
                return result;
            }
        }
        else if (!verificationMenuOpened && state.connection == Connection::Unknown && canVerifyThroughMenu(state))
        {
            if (!backend.invoke(Action::OpenMenu))
            {
                result.error = "Zoom's audio control could not be activated. The connection is unverified.";
                return result;
            }
            verificationMenuOpened = true;
        }
        backend.wait();
        result.observation = backend.read();
    }

    if (result.observation.joinComputerAvailable && result.actionAttempted)
        result.error = "The Join Audio by Computer button was clicked, but Zoom still shows it. "
                       "The connection is unverified and the meeting was not marked as joined.";
    else if (result.actionAttempted)
        result.error = "Join Audio by Computer was clicked, but Zoom did not confirm the connection. "
                       "The sound state is unverified.";
    else
        result.error = "Zoom's computer-audio connection could not be verified.";
    return result;
}

Result setConnected(const Backend& backend, bool connected, int attempts)
{
    if (connected)
        return joinComputerAudio(backend, attempts);

    Result result = readConnection(backend, attempts);
    if (!result.ok || result.observation.connection == Connection::Disconnected)
        return result;
    result.ok = false;
    if (!result.observation.leaveAvailable)
    {
        if (result.observation.menuAvailable && backend.invoke(Action::OpenMenu))
        {
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                backend.wait();
                result.observation = backend.read();
                if (result.observation.connection == Connection::Disconnected)
                {
                    result.ok = true;
                    return result;
                }
                if (result.observation.connection == Connection::Connected
                    && result.observation.leaveAvailable)
                    break;
            }
        }
        if (result.observation.connection != Connection::Connected
            || !result.observation.leaveAvailable)
        {
            result.error = "Zoom's Leave Computer Audio control could not be found. "
                           "The audio connection was not changed.";
            return result;
        }
    }

    result.actionAttempted = true;
    if (!backend.invoke(Action::LeaveAudio))
    {
        result.error = "Zoom's audio control could not be activated. The result is unverified.";
        return result;
    }

    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        backend.wait();
        result.observation = backend.read();
        if (result.observation.connection == Connection::Disconnected)
        {
            result.ok = true;
            result.changed = true;
            return result;
        }
    }
    result.error = "Zoom did not confirm the requested computer-audio state. "
                   "The result is unverified; check the meeting in Chrome.";
    return result;
}
}
