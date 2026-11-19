#include "ZoomAudioState.h"

namespace
{
bool namesAction(const QString& text, const QString& action)
{
    return text == action || text.startsWith(action + " (");
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
        || (microphoneReturned && !state.joinAvailable && !state.joinComputerAvailable));
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
    if (namesAction(text, "join audio") || namesAction(text, "connect audio"))
        return Action::JoinAudio;

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
        case Action::JoinAudio:
            result.joinAvailable = true;
            disconnectedControl = name;
            break;
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
            const QString state = microphoneState(name.simplified().toLower());
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

Result readConnection(const Backend& backend, int attempts)
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
        else if (!menuOpened && !tabSelected && state.computerAudioTabAvailable
                 && !state.computerAudioTabSelected && !state.computerAudioPanelVisible
                 && !state.joinComputerAvailable)
        {
            if (!backend.invoke(Action::SelectComputerAudio))
                break;
            tabSelected = true;
        }
        else if (!waitingForChooser && !menuOpened)
            break;
        backend.wait();
    }
    result.error = "Could not find the Join Audio or Leave Computer Audio button. "
                   "The sound state was not changed.";
    return result;
}

static Result joinComputerAudio(const Backend& backend, Result result, int attempts)
{
    // Zoom can open the chooser as soon as Sound Off finishes. Its toolbar
    // can update before its panel becomes accessible.
    constexpr int chooserSettleReads = 2;
    bool chooserSeen = false;
    bool toolbarInvoked = false;
    bool tabSelected = false;
    bool joinInvoked = false;
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

        chooserSeen = chooserSeen || state.chooserOpen();
        Action action = Action::None;
        if (!joinInvoked && state.joinComputerAvailable && state.connection == Connection::Disconnected)
        {
            action = Action::JoinComputerAudio;
            joinInvoked = true;
        }
        else if (!verificationMenuOpened && state.connection == Connection::Unknown
                 && canVerifyThroughMenu(state))
        {
            action = Action::OpenMenu;
            verificationMenuOpened = true;
        }
        else if (!joinInvoked && !tabSelected && state.computerAudioTabAvailable
                 && !state.computerAudioTabSelected && !state.computerAudioPanelVisible)
        {
            action = Action::SelectComputerAudio;
            tabSelected = true;
        }
        else if (!chooserSeen && !toolbarInvoked && state.joinAvailable
                 && state.connection == Connection::Disconnected
                 && attempt >= qMin(chooserSettleReads, attempts - 1))
        {
            action = Action::JoinAudio;
            toolbarInvoked = true;
        }

        if (action != Action::None)
        {
            if (action == Action::JoinComputerAudio || action == Action::JoinAudio)
                result.actionAttempted = true;
            if (!backend.invoke(action))
            {
                // A manual join or a Zoom re-render can remove the control
                // between discovery and activation. Verify before reporting
                // a failure or attributing a change to this request.
                if (action != Action::OpenMenu)
                {
                    Result current = readConnection(backend, attempts - attempt);
                    if (current.ok && current.observation.connection == Connection::Connected)
                    {
                        current.actionAttempted = result.actionAttempted;
                        return current;
                    }
                }
                result.error = action == Action::SelectComputerAudio
                    ? "Could not select the Computer Audio tab. The meeting was not joined."
                    : action == Action::JoinComputerAudio
                    ? "The Join Audio by Computer button was found, but it could not be clicked. "
                      "The connection is unverified and the meeting was not joined."
                    : action == Action::JoinAudio
                    ? "The Join Audio button was found, but it could not be clicked. "
                      "The connection is unverified and the meeting was not joined."
                    : "Zoom's audio control could not be activated. The connection is unverified.";
                return result;
            }
        }
        backend.wait();
        result.observation = backend.read();
    }

    if (result.observation.joinComputerAvailable && joinInvoked)
        result.error = "The Join Audio by Computer button was clicked, but Zoom still shows it. "
                       "The connection is unverified and the meeting was not marked as joined.";
    else if (!joinInvoked && toolbarInvoked)
        result.error = "Could not find the Join Audio by Computer button. "
                       "The Join Audio button was clicked, but that button did not appear. "
                       "The meeting was not joined.";
    else if (!joinInvoked && chooserSeen)
        result.error = "Could not find the Join Audio by Computer button in the audio chooser. "
                       "The meeting was not joined.";
    else
        result.error = "Could not find the Join Audio button. The meeting was not joined.";
    return result;
}

Result setConnected(const Backend& backend, bool connected, int attempts)
{
    Result result = readConnection(backend, attempts);
    const Connection wanted = connected ? Connection::Connected : Connection::Disconnected;
    if (!result.ok || result.observation.connection == wanted)
        return result;
    if (connected)
        return joinComputerAudio(backend, result, attempts);

    result.ok = false;
    if (!result.observation.leaveAvailable)
    {
        if (result.observation.menuAvailable && backend.invoke(Action::OpenMenu))
        {
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                backend.wait();
                result.observation = backend.read();
                if (result.observation.connection == wanted)
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
        if (result.observation.connection == wanted)
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
