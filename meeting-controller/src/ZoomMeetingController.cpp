#include "ZoomMeetingController.h"

#include "UiaSupport.h"
#include "ZoomAudioState.h"

#include <QThread>
#include <QElapsedTimer>
#include <QJsonArray>

#include <OleAuto.h>
#include <algorithm>
#include <numeric>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
// Zoom re-renders the control after a click, so the new state is polled rather
// than read once.
constexpr int kVerifyAttempts = 15;
constexpr int kVerifyIntervalMs = 200;
constexpr int kSelectSettleMs = 250;
constexpr int kAudioVerifyAttempts = 50;

QJsonObject failure(const QString& error)
{
    return QJsonObject{
        {"ok", false},
        {"error", error}
    };
}

// What the microphone control says about the meeting, named for the state Zoom
// is in rather than for the action the button offers.
enum class Microphone
{
    Unknown,
    Muted,
    Unmuted,
    AudioNotConnected
};

QString microphoneName(Microphone state)
{
    switch (state)
    {
    case Microphone::Muted: return "muted";
    case Microphone::Unmuted: return "unmuted";
    case Microphone::AudioNotConnected: return "audioNotConnected";
    default: return "unknown";
    }
}

// Zoom labels the button with the action it performs, so "Unmute" means the
// microphone is currently muted.
Microphone classifyButton(const QString& name)
{
    const QString text = name.trimmed().toLower();

    if (text.isEmpty())
        return Microphone::Unknown;

    if (text.startsWith("unmute"))
        return Microphone::Muted;

    // "Mute All" in the participants panel acts on everyone else.
    if (text.startsWith("mute") && !text.startsWith("mute all"))
        return Microphone::Unmuted;

    if (text.contains("join audio") || text.contains("connect audio"))
        return Microphone::AudioNotConnected;

    return Microphone::Unknown;
}

// A button named exactly "Mute" or "Unmute", or one that names the microphone,
// is the meeting control. Anything else that merely mentions muting is a weaker
// match and is only used when nothing better is found.
int matchStrength(const QString& name)
{
    const QString text = name.trimmed().toLower();

    if (text == "mute" || text == "unmute")
        return 3;

    if (text.contains("microphone") || text.contains("my audio"))
        return 2;

    return 1;
}

bool invokeElement(IUIAutomationElement* element)
{
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(element->GetCurrentPatternAs(
            UIA_InvokePatternId, IID_PPV_ARGS(invoke.GetAddressOf())))
        && invoke)
    {
        return SUCCEEDED(invoke->Invoke());
    }

    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(element->GetCurrentPatternAs(
            UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(legacy.GetAddressOf())))
        && legacy)
    {
        return SUCCEEDED(legacy->DoDefaultAction());
    }

    return false;
}

bool isVisibleEnabled(IUIAutomationElement* element)
{
    BOOL offscreen = TRUE;
    BOOL enabled = FALSE;
    return element
        && SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen
        && SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled;
}

bool sameElement(IUIAutomation* automation, IUIAutomationElement* a, IUIAutomationElement* b)
{
    BOOL same = FALSE;
    return a && b && SUCCEEDED(automation->CompareElements(a, b, &same)) && same;
}

bool withinElement(IUIAutomation* automation, IUIAutomationElement* child, IUIAutomationElement* parent)
{
    ComPtr<IUIAutomationTreeWalker> walker;
    if (!child || !parent || FAILED(automation->get_RawViewWalker(walker.GetAddressOf())) || !walker)
        return false;
    ComPtr<IUIAutomationElement> node = child;
    for (int depth = 0; node && depth < 96; ++depth)
    {
        if (sameElement(automation, node.Get(), parent))
            return true;
        ComPtr<IUIAutomationElement> next;
        if (FAILED(walker->GetParentElement(node.Get(), next.GetAddressOf())))
            break;
        node = std::move(next);
    }
    return false;
}

// Resolve the control at its current on-screen position. Chromium sometimes
// returns the button's text child from hit-testing, so accept its descendants.
std::optional<POINT> verifiedPoint(IUIAutomation* automation, HWND hwnd, IUIAutomationElement* element)
{
    RECT bounds{};
    if (GetForegroundWindow() != hwnd || FAILED(element->get_CurrentBoundingRectangle(&bounds))
        || IsRectEmpty(&bounds))
        return {};
    POINT point{};
    BOOL clickable = FALSE;
    if (FAILED(element->GetClickablePoint(&point, &clickable)) || !clickable || !PtInRect(&bounds, point))
        point = {std::midpoint(bounds.left, bounds.right), std::midpoint(bounds.top, bounds.bottom)};
    if (GetAncestor(WindowFromPhysicalPoint(point), GA_ROOT) != hwnd)
        return {};
    ComPtr<IUIAutomationElement> hit;
    if (FAILED(automation->ElementFromPoint(point, hit.GetAddressOf()))
        || !withinElement(automation, hit.Get(), element))
        return {};
    return point;
}

// A hosted renderer can answer hit tests while a search starting at Chrome's
// window still returns only the toolbar. Bind to the content underneath the
// middle of the meeting to query that fragment. This point is never clicked.
ComPtr<IUIAutomationElement> foregroundFragment(IUIAutomation* automation, HWND hwnd,
    IUIAutomationElement* document)
{
    RECT page{}, window{}, visible{};
    if (GetForegroundWindow() != hwnd || FAILED(document->get_CurrentBoundingRectangle(&page))
        || !GetWindowRect(hwnd, &window) || !IntersectRect(&visible, &page, &window))
        return {};
    const POINT middle{std::midpoint(visible.left, visible.right), std::midpoint(visible.top, visible.bottom)};
    const HWND content = WindowFromPhysicalPoint(middle);
    if (GetAncestor(content, GA_ROOT) != hwnd)
        return {};
    ComPtr<IUIAutomationElement> node;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (SUCCEEDED(automation->ElementFromPoint(middle, node.GetAddressOf()))
        && SUCCEEDED(automation->get_RawViewWalker(walker.GetAddressOf())) && walker)
    {
        for (int depth = 0; node && depth < 64; ++depth)
        {
            if (sameElement(automation, node.Get(), document))
                break;
            CONTROLTYPEID type = 0;
            node->get_CurrentControlType(&type);
            const QString role = uia::readStringProperty(node.Get(), UIA_AriaRolePropertyId);
            if (type == UIA_DocumentControlTypeId || role == "tabpanel" || role == "dialog"
                || uia::readStringProperty(node.Get(), UIA_AutomationIdPropertyId) == "voip-tab"
                || ((type == UIA_ButtonControlTypeId || role == "button") && zoomAudio::actionForName(
                    uia::readStringProperty(node.Get(), UIA_NamePropertyId)) == zoomAudio::Action::JoinComputerAudio))
                return node;
            ComPtr<IUIAutomationElement> parent;
            walker->GetParentElement(node.Get(), parent.GetAddressOf());
            node = std::move(parent);
        }
    }
    ComPtr<IUIAutomationElement> renderer;
    if (content != hwnd)
        automation->ElementFromHandle(content, renderer.GetAddressOf());
    return renderer;
}

bool clickJoinButton(POINT point)
{
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (point.x < left || point.x >= left + width || point.y < top || point.y >= top + height)
        return false;
    // Aim at the middle of a physical pixel, including on monitors to the
    // left of the primary display. Submit movement and click as one batch.
    INPUT events[3]{};
    for (INPUT& event : events)
        event.type = INPUT_MOUSE;
    events[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    events[0].mi.dx = static_cast<LONG>(((point.x - left) * 65536LL + 32768) / width);
    events[0].mi.dy = static_cast<LONG>(((point.y - top) * 65536LL + 32768) / height);
    events[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    events[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(3, events, sizeof(INPUT));
    if (sent == 2) // Release a button if Windows accepted only part of the batch.
        return SendInput(1, &events[2], sizeof(INPUT)) == 1;
    return sent == 3;
}

QString audioActionName(zoomAudio::Action action)
{
    switch (action)
    {
    case zoomAudio::Action::OpenMenu: return "open audio menu";
    case zoomAudio::Action::JoinAudio: return "open join-audio chooser";
    case zoomAudio::Action::JoinComputerAudio: return "join computer audio";
    case zoomAudio::Action::SelectComputerAudio: return "select Computer Audio tab";
    case zoomAudio::Action::LeaveAudio: return "leave computer audio";
    default: return "none";
    }
}
}

ZoomMeetingController::ZoomMeetingController(IUIAutomation* automation,
    std::function<void(const QString&)> diagnostic)
    : diagnostic_(std::move(diagnostic))
{
    automation_ = automation;
}

ComPtr<IUIAutomationElement> ZoomMeetingController::rootOf(HWND hwnd) const
{
    ComPtr<IUIAutomationElement> root;

    if (automation_ && IsWindow(hwnd))
        automation_->ElementFromHandle(hwnd, root.GetAddressOf());

    return root;
}

ComPtr<IUIAutomationElement> ZoomMeetingController::findTab(
    const ComPtr<IUIAutomationElement>& root,
    const QString& tabRuntimeId,
    const QString& tabName) const
{
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation_->get_ControlViewWalker(walker.GetAddressOf())) || !walker)
        return {};

    const uia::BrowserUi ui = uia::scanBrowserUi(walker.Get(), root.Get());

    for (const auto& item : ui.tabItems)
    {
        if (!tabRuntimeId.isEmpty() && uia::readRuntimeId(item.Get()) == tabRuntimeId)
            return item;
    }

    if (tabName.isEmpty())
        return {};

    // Chrome rebuilds tab elements when tabs are dragged or reordered, so the
    // runtime ID can go stale while the tab is still open.
    ComPtr<IUIAutomationElement> titleMatch;
    for (const auto& item : ui.tabItems)
    {
        if (uia::readStringProperty(item.Get(), UIA_NamePropertyId) == tabName)
        {
            if (titleMatch)
                return {}; // Duplicate titles cannot identify a meeting safely.
            titleMatch = item;
        }
    }
    return titleMatch;
}

ComPtr<IUIAutomationElement> ZoomMeetingController::findLiveDocument(HWND hwnd,
    const QString& selectedTabName) const
{
    ComPtr<IUIAutomationElement> root = rootOf(hwnd);
    if (!root)
        return {};

    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation_->get_ControlViewWalker(walker.GetAddressOf())) || !walker)
        return {};

    const uia::BrowserUi ui = uia::scanBrowserUi(walker.Get(), root.Get());

    ComPtr<IUIAutomationElement> first;
    ComPtr<IUIAutomationElement> named;
    int visibleCount = 0;
    int namedCount = 0;
    for (const auto& document : ui.documents)
    {
        if (uia::readBooleanProperty(document.Get(), UIA_IsOffscreenPropertyId))
            continue;
        if (selectedTabName.isEmpty())
            return document;
        ++visibleCount;
        if (!first)
            first = document;
        if (uia::readStringProperty(document.Get(), UIA_NamePropertyId) == selectedTabName)
        {
            named = document;
            ++namedCount;
        }
    }
    // A split Chrome window can expose two meetings at once.
    return visibleCount == 1 ? first : namedCount == 1 ? named : nullptr;
}

ComPtr<IUIAutomationElement> ZoomMeetingController::findMicrophoneButton(
    IUIAutomationElement* document) const
{
    VARIANT buttonType;
    VariantInit(&buttonType);
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;

    ComPtr<IUIAutomationCondition> condition;
    const HRESULT conditionHr = automation_->CreatePropertyCondition(
        UIA_ControlTypePropertyId, buttonType, condition.GetAddressOf());
    VariantClear(&buttonType);

    if (FAILED(conditionHr) || !condition)
        return {};

    ComPtr<IUIAutomationElementArray> buttons;
    if (FAILED(document->FindAll(
            TreeScope_Descendants, condition.Get(), buttons.GetAddressOf()))
        || !buttons)
        return {};

    int count = 0;
    buttons->get_Length(&count);

    ComPtr<IUIAutomationElement> best;
    int bestStrength = 0;

    for (int index = 0; index < count; ++index)
    {
        ComPtr<IUIAutomationElement> button;
        if (FAILED(buttons->GetElement(index, button.GetAddressOf())) || !button)
            continue;

        const QString name = uia::readStringProperty(button.Get(), UIA_NamePropertyId);
        if (classifyButton(name) == Microphone::Unknown)
            continue;

        const int strength = matchStrength(name);
        if (strength > bestStrength)
        {
            best = button;
            bestStrength = strength;
        }
    }

    return best;
}

QJsonObject ZoomMeetingController::readMicrophoneInWindow(HWND hwnd) const
{
    ComPtr<IUIAutomationElement> document = findLiveDocument(hwnd);
    if (!document)
    {
        return failure(
            "The page content of that tab is not available through UI Automation.");
    }

    ComPtr<IUIAutomationElement> button = findMicrophoneButton(document.Get());
    if (!button)
    {
        return failure(
            "No microphone control was found on that page. Open the meeting tab and "
            "make sure the Zoom toolbar is visible.");
    }

    const QString name = uia::readStringProperty(button.Get(), UIA_NamePropertyId);

    return QJsonObject{
        {"ok", true},
        {"state", microphoneName(classifyButton(name))},
        {"controlName", name}
    };
}

QJsonObject ZoomMeetingController::activateTab(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName,
    bool bringToFront) const
{
    if (!automation_)
        return failure("UI Automation is not initialized.");

    HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    if (!IsWindow(hwnd))
        return failure("That Chrome window is no longer open.");

    ComPtr<IUIAutomationElement> root = rootOf(hwnd);
    if (!root)
        return failure("The Chrome window could not be read through UI Automation.");

    ComPtr<IUIAutomationElement> tab = findTab(root, tabRuntimeId, tabName);
    if (!tab)
        return failure("That tab is no longer open in its Chrome window.");

    bool selected = uia::readBooleanProperty(
        tab.Get(), UIA_SelectionItemIsSelectedPropertyId);

    ComPtr<IUIAutomationSelectionItemPattern> selection;
    if (!selected && SUCCEEDED(tab->GetCurrentPatternAs(
            UIA_SelectionItemPatternId, IID_PPV_ARGS(selection.GetAddressOf())))
        && selection)
    {
        if (FAILED(selection->Select()))
            return failure("Chrome refused to switch to that tab.");

        QThread::msleep(kSelectSettleMs);
        selected = uia::readBooleanProperty(
            tab.Get(), UIA_SelectionItemIsSelectedPropertyId);
    }

    if (bringToFront)
    {
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);

        SetForegroundWindow(hwnd);
    }

    if (!selected)
        return failure("The tab did not become the selected tab in its window.");

    return QJsonObject{
        {"ok", true},
        {"tabRuntimeId", uia::readRuntimeId(tab.Get())},
        {"tabName", uia::readStringProperty(tab.Get(), UIA_NamePropertyId)}
    };
}

QJsonObject ZoomMeetingController::activateAndReadMicrophone(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName) const
{
    const QJsonObject activation =
        activateTab(hwndValue, tabRuntimeId, tabName, true);

    if (!activation.value("ok").toBool())
        return activation;

    QJsonObject result = readMicrophoneInWindow(reinterpret_cast<HWND>(hwndValue));
    const QJsonObject audio = readAudioConnection(
        hwndValue, activation.value("tabRuntimeId").toString(), tabName);
    if (audio.value("ok").toBool())
        result = audio;
    else
    {
        // A missing audio-menu name must not discard an independently readable
        // microphone during import. Sound stays unread until it can be verified.
        result.insert("audioState", "unknown");
        result.insert("audioError", audio.value("error"));
    }
    result.insert("activated", true);
    result.insert("tabRuntimeId", audio.value("tabRuntimeId").toString().isEmpty()
        ? activation.value("tabRuntimeId") : audio.value("tabRuntimeId"));
    result.insert("tabName", activation.value("tabName"));
    return result;
}

std::vector<ZoomMeetingController::Control> ZoomMeetingController::collectControls(
    IUIAutomationElement* root, HWND hwnd, IUIAutomationElement* popupDocument) const
{
    std::vector<Control> controls;
    if (!automation_ || !root)
        return controls;

    ComPtr<IUIAutomationCondition> condition;
    const auto include = [&](PROPERTYID property, const VARIANT& value)
    {
        ComPtr<IUIAutomationCondition> term;
        if (FAILED(automation_->CreatePropertyCondition(property, value, term.GetAddressOf())))
            return false;
        if (!condition)
            condition = std::move(term);
        else
        {
            ComPtr<IUIAutomationCondition> combined;
            if (FAILED(automation_->CreateOrCondition(condition.Get(), term.Get(), combined.GetAddressOf())))
                return false;
            condition = std::move(combined);
        }
        return true;
    };
    const auto includeText = [&](PROPERTYID property, const wchar_t* text)
    {
        VARIANT value{};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(text);
        const bool ok = value.bstrVal && include(property, value);
        VariantClear(&value);
        return ok;
    };
    for (const CONTROLTYPEID type : {UIA_ButtonControlTypeId, UIA_MenuItemControlTypeId,
                                     UIA_SplitButtonControlTypeId, UIA_TabItemControlTypeId})
    {
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = type;
        if (!include(UIA_ControlTypePropertyId, value))
            return controls;
    }
    if (!includeText(UIA_AriaRolePropertyId, L"button") || !includeText(UIA_AriaRolePropertyId, L"tab")
        || !includeText(UIA_AutomationIdPropertyId, L"voip-tab"))
        return controls;

    // Search the raw view in one provider call. The Zoom chooser can sit
    // under an ordinary group with no dialog or Document role.
    ComPtr<IUIAutomationCacheRequest> cache;
    ComPtr<IUIAutomationCondition> raw;
    if (FAILED(automation_->CreateCacheRequest(cache.GetAddressOf())) || !cache
        || FAILED(automation_->get_RawViewCondition(raw.GetAddressOf()))
        || FAILED(cache->put_TreeFilter(raw.Get())))
        return controls;
    cache->put_TreeScope(TreeScope_Element);
    for (const PROPERTYID property : {UIA_NamePropertyId, UIA_ControlTypePropertyId,
            UIA_IsOffscreenPropertyId, UIA_IsEnabledPropertyId, UIA_AutomationIdPropertyId,
            UIA_AriaRolePropertyId, UIA_SelectionItemIsSelectedPropertyId})
        cache->AddProperty(property);

    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root->FindAllBuildCache(TreeScope_Subtree, condition.Get(), cache.Get(),
                                       elements.GetAddressOf())) || !elements)
        return controls;

    int count = 0;
    elements->get_Length(&count);
    for (int index = 0; index < count; ++index)
    {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, element.GetAddressOf())) || !element)
            continue;
        BOOL enabled = FALSE;
        if (FAILED(element->get_CachedIsEnabled(&enabled)) || !enabled)
            continue;
        const auto cachedText = [&](PROPERTYID property)
        {
            VARIANT value{};
            QString text;
            if (SUCCEEDED(element->GetCachedPropertyValue(property, &value))
                && value.vt == VT_BSTR && value.bstrVal)
                text = QString::fromWCharArray(value.bstrVal, SysStringLen(value.bstrVal));
            VariantClear(&value);
            return text;
        };
        CONTROLTYPEID type = 0;
        element->get_CachedControlType(&type);
        const QString name = cachedText(UIA_NamePropertyId);
        const auto action = zoomAudio::actionForName(name);
        const QString role = cachedText(UIA_AriaRolePropertyId);
        if (role == "tab")
            type = UIA_TabItemControlTypeId;
        else if (role == "button" && type != UIA_SplitButtonControlTypeId && type != UIA_MenuItemControlTypeId)
            type = UIA_ButtonControlTypeId;
        const bool panel = cachedText(UIA_AutomationIdPropertyId) == "voip-tab"
            && (role == "tabpanel" || name.compare("Computer Audio", Qt::CaseInsensitive) == 0);
        if (!panel && ((action == zoomAudio::Action::SelectComputerAudio) != (type == UIA_TabItemControlTypeId)
            || (action == zoomAudio::Action::None && zoomAudio::observe({name}).microphoneState == "unknown")))
            continue;

        const bool chooserControl = panel || action == zoomAudio::Action::JoinComputerAudio
            || action == zoomAudio::Action::SelectComputerAudio;
        BOOL offscreen = TRUE;
        if (FAILED(element->get_CachedIsOffscreen(&offscreen)) || offscreen)
        {
            // Chrome often marks Zoom's join button offscreen even while its
            // box is inside the meeting window. A display:none copy has no box.
            RECT bounds{}, window{}, visible{};
            const bool placed = chooserControl
                && SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds))
                && !IsRectEmpty(&bounds)
                && GetWindowRect(hwnd, &window)
                && IntersectRect(&visible, &bounds, &window);
            if (!placed && (!chooserControl || !verifiedPoint(automation_.Get(), hwnd, element.Get())))
                continue;
        }
        if (popupDocument && !withinElement(automation_.Get(), element.Get(), popupDocument))
        {
            if (!chooserControl && action != zoomAudio::Action::LeaveAudio)
                continue;
            // Detached overlay: the named control must occupy the selected
            // meeting's content area, including when Chrome is in split view.
            RECT page{}, item{};
            if (FAILED(popupDocument->get_CurrentBoundingRectangle(&page))
                || FAILED(element->get_CurrentBoundingRectangle(&item)) || IsRectEmpty(&item)
                || !PtInRect(&page, {std::midpoint(item.left, item.right), std::midpoint(item.top, item.bottom)}))
                continue;
        }
        VARIANT selected{};
        const bool isSelected = type == UIA_TabItemControlTypeId
            && SUCCEEDED(element->GetCachedPropertyValue(UIA_SelectionItemIsSelectedPropertyId, &selected))
            && selected.vt == VT_BOOL && selected.boolVal == VARIANT_TRUE;
        VariantClear(&selected);
        controls.push_back({element, name, type, isSelected, panel});
    }
    return controls;
}

QJsonObject ZoomMeetingController::controlAudioConnection(
    quintptr hwndValue, const QString& tabRuntimeId, const QString& tabName,
    std::optional<bool> connected) const
{
    QElapsedTimer timer;
    timer.start();
    QJsonArray diagnostics;
    const auto report = [&](const QString& message)
    {
        const QString line = QString("Audio +%1 ms: %2").arg(timer.elapsed()).arg(message);
        if (diagnostics.size() < 80)
            diagnostics.append(line);
        if (diagnostic_)
            diagnostic_(line);
    };
    report(connected.has_value() ? (*connected ? "Sound On requested" : "Sound Off requested")
                                : "Reading audio connection");
    // Zoom's join chooser and browser audio activation must be on screen when
    // reconnecting, rather than acting behind the controller/minimized Chrome.
    const QJsonObject activation = activateTab(hwndValue, tabRuntimeId, tabName,
                                               connected.value_or(false));
    if (!activation.value("ok").toBool())
        return activation;

    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    const QString selectedId = activation.value("tabRuntimeId").toString();
    QString contextError;
    bool targetLost = false;
    // Discover an already-open chooser before clicking Join Audio; that
    // toolbar button can otherwise just reopen/toggle the same dialog.
    bool searchPopups = connected.value_or(false);
    bool openedMenu = false;
    QString menuRuntimeId;
    QString menuName;
    const auto windowRoot = rootOf(hwnd);
    const auto selectedTab = windowRoot ? findTab(windowRoot, selectedId, QString()) : nullptr;
    const QString selectedName = activation.value("tabName").toString();
    ComPtr<IUIAutomationElement> document;
    QString lastObservation;

    const auto targetSelected = [&]
    {
        if (targetLost)
            return false;
        if (!IsWindow(hwnd) || !selectedTab
            || !uia::readBooleanProperty(selectedTab.Get(), UIA_SelectionItemIsSelectedPropertyId))
        {
            targetLost = true;
            contextError = "The meeting tab closed or is no longer selected. "
                           "The sound result is unverified; check the meeting in Chrome.";
            return false;
        }
        return true;
    };
    const auto targetInWindow = [&]
    {
        if (!targetSelected())
            return false;
        const auto root = rootOf(hwnd);
        if (!root || !findTab(root, selectedId, QString()))
        {
            targetLost = true;
            contextError = "The meeting tab moved or closed. The sound result is unverified.";
            return false;
        }
        return true;
    };
    const auto readControls = [&]
    {
        std::vector<Control> controls;
        if (!targetSelected())
            return controls;
        if (!document)
            document = findLiveDocument(hwnd, selectedName);
        if (!document)
            return controls;
        controls = collectControls(document.Get(), hwnd);
        const auto appendFragment = [&](IUIAutomationElement* fragment)
        {
            auto popupControls = collectControls(fragment, hwnd, document.Get());
            for (const Control& candidate : popupControls)
            {
                const bool duplicate = std::any_of(controls.cbegin(), controls.cend(),
                    [&](const Control& existing)
                    {
                        return existing.name == candidate.name && existing.type == candidate.type
                            && sameElement(automation_.Get(), existing.element.Get(), candidate.element.Get());
                    });
                if (!duplicate)
                    controls.push_back(candidate);
            }
        };
        if (searchPopups && windowRoot)
            appendFragment(windowRoot.Get());
        if (connected.value_or(false))
        {
            bool chooserExpected = false;
            bool joinFound = false;
            for (const Control& control : controls)
            {
                const auto action = zoomAudio::actionForName(control.name);
                chooserExpected |= control.computerAudioPanel || action == zoomAudio::Action::SelectComputerAudio
                    || action == zoomAudio::Action::JoinAudio;
                joinFound |= action == zoomAudio::Action::JoinComputerAudio;
            }
            if (chooserExpected && !joinFound)
            {
                const auto fragment = foregroundFragment(automation_.Get(), hwnd, document.Get());
                if (fragment && !sameElement(automation_.Get(), fragment.Get(), windowRoot.Get()))
                    appendFragment(fragment.Get());
            }
        }
        if (controls.empty())
            document.Reset();
        if (!targetSelected())
            return std::vector<Control>();
        return controls;
    };
    const auto observeControls = [&](const std::vector<Control>& controls)
    {
        QStringList names;
        QStringList audioNames;
        bool panelVisible = false;
        for (const Control& control : controls)
        {
            if (control.computerAudioPanel)
            {
                panelVisible = true;
                audioNames.append("Computer Audio panel");
                continue;
            }
            names.append(control.name);
            if (zoomAudio::actionForName(control.name) != zoomAudio::Action::None
                || zoomAudio::observe({control.name}).microphoneState != "unknown")
                audioNames.append(control.name);
        }
        auto observation = zoomAudio::observe(names);
        observation.computerAudioPanelVisible = panelVisible;
        for (const Control& control : controls)
        {
            if (control.type == UIA_TabItemControlTypeId && control.selected
                && zoomAudio::actionForName(control.name) == zoomAudio::Action::SelectComputerAudio)
                observation.computerAudioTabSelected = true;
        }
        const QString summary = QString("Read %1; microphone %2; controls [%3]; computer tab %4")
            .arg(zoomAudio::connectionName(observation.connection), observation.microphoneState,
                 audioNames.join(" | "), observation.computerAudioTabSelected ? "selected" : "not selected");
        if (summary != lastObservation)
        {
            report(summary);
            lastObservation = summary;
        }
        return observation;
    };
    const auto uniqueControl = [&](const std::vector<Control>& controls, zoomAudio::Action action)
    {
        ComPtr<IUIAutomationElement> match;
        for (const Control& control : controls)
        {
            if (control.computerAudioPanel || zoomAudio::actionForName(control.name) != action)
                continue;
            if (action == zoomAudio::Action::OpenMenu && control.type == UIA_MenuItemControlTypeId)
                continue; // Audio Settings inside the menu is not its opener.
            if (action == zoomAudio::Action::SelectComputerAudio && control.type != UIA_TabItemControlTypeId)
                continue;
            if (match)
            {
                contextError = "More than one Zoom audio control matched. "
                               "The sound result is unverified; inspect the meeting in Live Diagnostics.";
                return ComPtr<IUIAutomationElement>();
            }
            match = control.element;
        }
        return match;
    };

    const zoomAudio::Backend backend{
        [&]
        {
            return observeControls(readControls());
        },
        [&](zoomAudio::Action action)
        {
            report("Preparing to " + audioActionName(action));
            const auto controls = readControls();
            const auto observation = observeControls(controls);
            if (action == zoomAudio::Action::JoinAudio && observation.chooserOpen())
            {
                report("The chooser appeared before the toolbar action. Using the open chooser.");
                return true;
            }
            // State may have changed since the previous read. Never activate a
            // connection control on contradictory or newly unknown evidence.
            if ((action == zoomAudio::Action::LeaveAudio
                 && observation.connection != zoomAudio::Connection::Connected)
                || ((action == zoomAudio::Action::JoinAudio
                     || action == zoomAudio::Action::JoinComputerAudio)
                    && observation.connection != zoomAudio::Connection::Disconnected))
                return false;

            const auto element = uniqueControl(controls, action);
            if (!element || !targetInWindow())
            {
                report("Could not identify an actionable control to " + audioActionName(action));
                return false;
            }
            const bool joining = action == zoomAudio::Action::JoinComputerAudio;
            if (!uia::readBooleanProperty(element.Get(), UIA_IsEnabledPropertyId))
                return false;
            if (!joining && !isVisibleEnabled(element.Get()) && !(action == zoomAudio::Action::SelectComputerAudio
                && verifiedPoint(automation_.Get(), hwnd, element.Get())))
                return false;
            bool invoked = false;
            if (action == zoomAudio::Action::SelectComputerAudio)
            {
                if (uia::readBooleanProperty(element.Get(), UIA_SelectionItemIsSelectedPropertyId))
                    return true;
                ComPtr<IUIAutomationSelectionItemPattern> selection;
                invoked = SUCCEEDED(element->GetCurrentPatternAs(
                    UIA_SelectionItemPatternId, IID_PPV_ARGS(selection.GetAddressOf())))
                    && selection && SUCCEEDED(selection->Select());
            }
            else if (action == zoomAudio::Action::OpenMenu)
            {
                ComPtr<IUIAutomationExpandCollapsePattern> expand;
                if (SUCCEEDED(element->GetCurrentPatternAs(
                        UIA_ExpandCollapsePatternId, IID_PPV_ARGS(expand.GetAddressOf()))) && expand)
                {
                    ExpandCollapseState state = ExpandCollapseState_LeafNode;
                    if (FAILED(expand->get_CurrentExpandCollapseState(&state)))
                        return false;
                    if (state == ExpandCollapseState_Expanded)
                    {
                        searchPopups = true;
                        return true;
                    }
                    invoked = SUCCEEDED(expand->Expand());
                }
                else
                    invoked = invokeElement(element.Get());
                openedMenu = invoked;
                if (invoked)
                {
                    menuRuntimeId = uia::readRuntimeId(element.Get());
                    menuName = uia::readStringProperty(element.Get(), UIA_NamePropertyId);
                }
            }
            else if (joining)
            {
                if (!targetSelected() || zoomAudio::actionForName(uia::readStringProperty(
                        element.Get(), UIA_NamePropertyId)) != zoomAudio::Action::JoinComputerAudio)
                    return false;
                const auto point = verifiedPoint(automation_.Get(), hwnd, element.Get());
                if (point)
                {
                    if (!targetSelected() || GetForegroundWindow() != hwnd
                        || GetAncestor(WindowFromPhysicalPoint(*point), GA_ROOT) != hwnd
                        || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
                        return false;
                    report("Clicking the verified Join Audio by Computer button in Chrome");
                    invoked = clickJoinButton(*point);
                }
                else if (targetSelected()
                    && uia::readBooleanProperty(element.Get(), UIA_IsEnabledPropertyId))
                {
                    report("Using the Join Audio by Computer button's accessible action");
                    invoked = invokeElement(element.Get());
                }
            }
            else
                invoked = invokeElement(element.Get());
            searchPopups = searchPopups || invoked;
            if (invoked)
                document.Reset();
            report(QString("%1: %2").arg(audioActionName(action), invoked ? "action returned success" : "action failed"));
            return invoked;
        },
        [] { QThread::msleep(kVerifyIntervalMs); }
    };

    report("Reading the current Zoom audio controls");
    const zoomAudio::Result outcome = connected.has_value()
        ? zoomAudio::setConnected(backend, *connected, kAudioVerifyAttempts)
        : zoomAudio::readConnection(backend, kVerifyAttempts);

    // Close a menu opened for discovery if it is still present. Never dismiss a
    // join dialog or issue a blind Escape/click on an unrelated page.
    if (openedMenu && targetInWindow())
    {
        const auto controls = readControls();
        bool leaveMenuVisible = false;
        for (const Control& control : controls)
        {
            if (zoomAudio::actionForName(control.name) == zoomAudio::Action::LeaveAudio)
                leaveMenuVisible = true;
        }
        ComPtr<IUIAutomationElement> opener;
        for (const Control& control : controls)
        {
            if (!menuRuntimeId.isEmpty() && uia::readRuntimeId(control.element.Get()) == menuRuntimeId)
            {
                opener = control.element;
                break;
            }
        }
        if (!opener && !menuName.isEmpty())
        {
            for (const Control& control : controls)
            {
                if (control.name == menuName)
                {
                    if (opener)
                    {
                        opener.Reset();
                        break;
                    }
                    opener = control.element;
                }
            }
        }
        if (opener && targetInWindow())
        {
            ComPtr<IUIAutomationExpandCollapsePattern> expand;
            if (SUCCEEDED(opener->GetCurrentPatternAs(
                    UIA_ExpandCollapsePatternId, IID_PPV_ARGS(expand.GetAddressOf()))) && expand)
            {
                ExpandCollapseState state = ExpandCollapseState_LeafNode;
                if (SUCCEEDED(expand->get_CurrentExpandCollapseState(&state))
                    && state == ExpandCollapseState_Expanded)
                    expand->Collapse();
            }
            else if (leaveMenuVisible)
                invokeElement(opener.Get());
        }
    }

    const bool ok = outcome.ok && targetInWindow() && contextError.isEmpty();
    report(ok ? "Audio state verified" : "Audio state unverified");
    QJsonObject result{
        {"ok", ok}, {"verified", ok}, {"changed", ok && outcome.changed},
        {"stateChangeAttempted", outcome.actionAttempted},
        {"audioState", ok ? zoomAudio::connectionName(outcome.observation.connection) : "unknown"},
        {"audioControlName", outcome.observation.controlName},
        {"state", ok ? outcome.observation.microphoneState : "unknown"},
        {"controlName", outcome.observation.microphoneControlName},
        {"tabRuntimeId", selectedId}, {"audioDiagnostics", diagnostics}
    };
    if (!ok)
        result.insert("error", contextError.isEmpty() ? outcome.error : contextError);
    return result;
}

QJsonObject ZoomMeetingController::readAudioConnection(
    quintptr hwndValue, const QString& tabRuntimeId, const QString& tabName) const
{
    return controlAudioConnection(hwndValue, tabRuntimeId, tabName, std::nullopt);
}

QJsonObject ZoomMeetingController::setAudioConnected(
    quintptr hwndValue, const QString& tabRuntimeId, const QString& tabName, bool connected) const
{
    return controlAudioConnection(hwndValue, tabRuntimeId, tabName, connected);
}

QJsonObject ZoomMeetingController::readMicrophone(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName) const
{
    // Chrome only renders the selected tab, so the meeting has to be on screen
    // in its own window before its controls can be read.
    const QJsonObject activation =
        activateTab(hwndValue, tabRuntimeId, tabName, false);

    if (!activation.value("ok").toBool())
        return activation;

    QJsonObject result = readMicrophoneInWindow(reinterpret_cast<HWND>(hwndValue));
    if (result.value("ok").toBool())
        result.insert("tabRuntimeId", activation.value("tabRuntimeId"));

    return result;
}

QJsonObject ZoomMeetingController::setMicrophoneMuted(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName,
    bool muted) const
{
    const QJsonObject activation =
        activateTab(hwndValue, tabRuntimeId, tabName, false);

    if (!activation.value("ok").toBool())
        return activation;

    HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    const QString wanted = muted ? "muted" : "unmuted";

    const QJsonObject before = readMicrophoneInWindow(hwnd);
    if (!before.value("ok").toBool())
        return before;

    const QString current = before.value("state").toString();

    if (current == "audioNotConnected")
    {
        return failure(
            "That meeting has not joined audio yet, so its microphone cannot be "
            "changed. Join audio in Zoom first.");
    }

    if (current == "unknown")
    {
        return failure(
            "The microphone control was found but its state could not be read, so "
            "nothing was changed.");
    }

    if (current == wanted)
    {
        QJsonObject result = before;
        result.insert("changed", false);
        result.insert("verified", true);
        result.insert("tabRuntimeId", activation.value("tabRuntimeId"));
        return result;
    }

    ComPtr<IUIAutomationElement> document = findLiveDocument(hwnd);
    ComPtr<IUIAutomationElement> button =
        document ? findMicrophoneButton(document.Get()) : nullptr;

    if (!button || !invokeElement(button.Get()))
    {
        return failure(
            "The microphone control does not support being activated through UI "
            "Automation, so nothing was changed.");
    }

    for (int attempt = 0; attempt < kVerifyAttempts; ++attempt)
    {
        QThread::msleep(kVerifyIntervalMs);

        const QJsonObject after = readMicrophoneInWindow(hwnd);
        if (!after.value("ok").toBool())
            continue;

        if (after.value("state").toString() == wanted)
        {
            QJsonObject result = after;
            result.insert("changed", true);
            result.insert("verified", true);
            result.insert("tabRuntimeId", activation.value("tabRuntimeId"));
            return result;
        }
    }

    const QJsonObject unverified = readMicrophoneInWindow(hwnd);

    QJsonObject result = failure(
        "The microphone control was activated but Zoom did not report the new "
        "state, so the result is unverified.");
    result.insert("state", unverified.value("ok").toBool()
        ? unverified.value("state")
        : QJsonValue("unknown"));
    result.insert("verified", false);
    return result;
}
