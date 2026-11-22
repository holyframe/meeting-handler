#include "WindowsLiveCaptions.h"
#include "captions/CaptionSource.h"

#include <shellapi.h>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QThread>
#include <vector>

namespace windows_captions {
namespace {
using Microsoft::WRL::ComPtr;
using Toggle = ComPtr<IUIAutomationTogglePattern>;

QString settingsImage() {
    wchar_t directory[MAX_PATH]{};
    const UINT length = ::GetWindowsDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    return QString::fromWCharArray(directory) + QStringLiteral("\\ImmersiveControlPanel\\SystemSettings.exe");
}

bool isSettingsProcess(DWORD pid) {
    static const QString expected = settingsImage();
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const bool read = ::QueryFullProcessImageNameW(process, 0, path, &length);
    ::CloseHandle(process);
    return read && expected.compare(QString::fromWCharArray(path, length), Qt::CaseInsensitive) == 0;
}

bool isSettingsWindow(HWND window) {
    DWORD pid = 0;
    return window && ::GetWindowThreadProcessId(window, &pid) && isSettingsProcess(pid);
}

BOOL CALLBACK findSettingsChild(HWND child, LPARAM context) {
    if (!isSettingsWindow(child)) return TRUE;
    *reinterpret_cast<bool*>(context) = true;
    return FALSE;
}

BOOL CALLBACK collectSettingsFrames(HWND window, LPARAM context) {
    if (!::IsWindowVisible(window)) return TRUE;
    bool hostsSettings = isSettingsWindow(window);
    if (!hostsSettings) {
        wchar_t name[64]{};
        ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
        if (wcscmp(name, L"ApplicationFrameWindow") == 0)
            ::EnumChildWindows(window, findSettingsChild, reinterpret_cast<LPARAM>(&hostsSettings));
    }
    if (hostsSettings)
        reinterpret_cast<std::vector<HWND>*>(context)->push_back(window);
    return TRUE;
}

Toggle findSwitch(IUIAutomation* automation) {
    std::vector<HWND> frames;
    ::EnumWindows(collectSettingsFrames, reinterpret_cast<LPARAM>(&frames));
    for (HWND window : frames) {
        ComPtr<IUIAutomationElement> root, control;
        if (FAILED(automation->ElementFromHandle(window, &root)) || !root) continue;
        // Resolve an element first so UI Automation has initialized its property schema.
        VARIANT value{};
        value.vt = VT_BSTR;
        value.bstrVal = ::SysAllocString(L"SystemSettings_Accessibility_Caption_LCToggle_ToggleSwitch");
        if (!value.bstrVal) return {};
        ComPtr<IUIAutomationCondition> id;
        const HRESULT hr = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &id);
        ::VariantClear(&value);
        if (FAILED(hr) || !id ||
            FAILED(root->FindFirst(TreeScope_Descendants, id.Get(), &control)) || !control) continue;
        int pid = 0;
        BOOL enabled = FALSE;
        if (FAILED(control->get_CurrentProcessId(&pid)) || !isSettingsProcess(pid) ||
            FAILED(control->get_CurrentIsEnabled(&enabled)) || !enabled) continue;
        Toggle toggle;
        if (SUCCEEDED(control->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) && toggle)
            return toggle;
    }
    return {};
}

std::optional<bool> readState(const Toggle& control) {
    ToggleState state = ToggleState_Indeterminate;
    if (!control || FAILED(control->get_CurrentToggleState(&state)) || state == ToggleState_Indeterminate)
        return std::nullopt;
    return state == ToggleState_On;
}

Result changeSetting(bool desired, std::stop_token stop) {
    ComPtr<IUIAutomation> automation;
    if (FAILED(::CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
        return {{}, QObject::tr("Windows UI Automation is unavailable.")};
    ComPtr<IUIAutomation2> timeouts;
    if (SUCCEEDED(automation.As(&timeouts))) {
        timeouts->put_ConnectionTimeout(1000);
        timeouts->put_TransactionTimeout(1500);
    }
    Toggle control = findSwitch(automation.Get());
    auto state = readState(control);
    if (!state && !stop.stop_requested()) {
        const auto uri = QString::fromLatin1(settingsUri).toStdWString();
        if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            return {{}, QObject::tr("Windows could not open Captions settings.")};
        QDeadlineTimer deadline(6000);
        while (!state && !stop.stop_requested() && !deadline.hasExpired()) {
            QThread::msleep(120);
            control = findSwitch(automation.Get());
            state = readState(control);
        }
    }
    if (stop.stop_requested()) return {};
    if (!state) return {{}, QObject::tr("Windows did not expose the Live Captions switch. Check Captions settings for availability or setup.")};
    if (*state == desired) return {state, {}};

    // A provider can apply a toggle even if its reply is lost. Send it once, then only read.
    control->Toggle();
    QDeadlineTimer deadline(5000);
    while (!stop.stop_requested()) {
        state = readState(control);
        if (state == desired) return {state, {}};
        if (deadline.hasExpired()) break;
        QThread::msleep(120);
        if (!state) control = findSwitch(automation.Get());
    }
    return {state, QObject::tr("Windows did not confirm the requested Live Captions state. Check the switch in Captions settings.")};
}

Result setEnabled(bool desired, std::stop_token stop) {
    if (stop.stop_requested()) return {};
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return {{}, QObject::tr("Could not connect to Windows accessibility services.")};
    const Result result = changeSetting(desired, stop);
    ::CoUninitialize();
    return result;
}
}

Backend systemBackend() {
    return {CaptionSource::WindowsLiveCaptionsWindowOpen, setEnabled};
}
}
