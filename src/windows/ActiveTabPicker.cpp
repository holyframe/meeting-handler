#include "ActiveTabPicker.h"

#include "ChromeWindowEnumerator.h"
#include "UiaSupport.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
using uia::BrowserUi;
using uia::readBooleanProperty;
using uia::readRuntimeId;
using uia::readStringProperty;
using uia::scanBrowserUi;

QUrl parseBrowserUrl(QString text)
{
    static const QRegularExpression whitespace("\\s");

    text = text.trimmed();
    if (text.isEmpty() || text.contains(whitespace))
        return {};

    // Chrome's address bar hides the https:// scheme.
    if (!text.contains("://"))
        text.prepend("https://");

    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || !url.host().contains('.'))
        return {};

    return url;
}

bool isZoomHost(const QString& host)
{
    static const QStringList domains{"zoom.us", "zoom.com", "zoomgov.com"};

    for (const QString& domain : domains)
    {
        if (host.compare(domain, Qt::CaseInsensitive) == 0
            || host.endsWith("." + domain, Qt::CaseInsensitive))
            return true;
    }

    return false;
}

QString zoomMeetingId(const QUrl& url)
{
    static const QRegularExpression meetingPath(
        "^/wc/(?:join/)?(\\d{9,12})(?:/|$)");

    const QRegularExpressionMatch match = meetingPath.match(url.path());
    return match.hasMatch() ? match.captured(1) : QString();
}

QJsonObject failure(const QString& error)
{
    return QJsonObject{
        {"ok", false},
        {"error", error}
    };
}

void applyAddress(QJsonObject& tab, const QUrl& url, const QString& source)
{
    tab.insert("addressKnown", true);
    tab.insert("urlSource", source);
    tab.insert("host", url.host());

    const QString meetingId = isZoomHost(url.host()) ? zoomMeetingId(url) : QString();
    tab.insert("zoomMeeting", !meetingId.isEmpty());
    if (!meetingId.isEmpty())
        tab.insert("meetingId", meetingId);
}

QUrl readWindowAddress(const BrowserUi& ui, QString* source)
{
    for (const auto& edit : ui.edits)
    {
        const QUrl url = parseBrowserUrl(
            readStringProperty(edit.Get(), UIA_ValueValuePropertyId));
        if (url.isValid())
        {
            *source = "addressBar";
            return url;
        }
    }

    for (const auto& document : ui.documents)
    {
        if (readBooleanProperty(document.Get(), UIA_IsOffscreenPropertyId))
            continue;

        const QUrl url = parseBrowserUrl(
            readStringProperty(document.Get(), UIA_ValueValuePropertyId));
        if (url.isValid())
        {
            *source = "document";
            return url;
        }
    }

    return {};
}

QJsonObject makeTabRecord(
    IUIAutomationElement* element,
    const QJsonObject& window,
    bool selected,
    const QUrl& windowUrl,
    const QString& windowUrlSource,
    int index)
{
    QJsonObject tab;
    tab.insert("windowHandle", window.value("hwnd"));
    tab.insert("windowTitle", window.value("title"));
    tab.insert("processId", window.value("processId"));
    tab.insert("profileName", window.value("profileName"));
    tab.insert("profileKey", window.value("profileKey"));

    const QString name = readStringProperty(element, UIA_NamePropertyId);
    tab.insert("tabName", name.isEmpty() ? window.value("title").toString() : name);

    QString runtimeId = readRuntimeId(element);
    if (runtimeId.isEmpty())
        runtimeId = QString("index-%1").arg(index);
    tab.insert("tabRuntimeId", runtimeId);
    tab.insert("selected", selected);
    tab.insert("addressKnown", false);
    tab.insert("zoomMeeting", false);

    const QUrl ownUrl = parseBrowserUrl(
        readStringProperty(element, UIA_ValueValuePropertyId));
    if (ownUrl.isValid())
        applyAddress(tab, ownUrl, "tab");
    else if (selected && windowUrl.isValid())
        applyAddress(tab, windowUrl, windowUrlSource);

    return tab;
}
}

ActiveTabPicker::ActiveTabPicker(IUIAutomation* automation)
{
    automation_ = automation;
}

QJsonObject ActiveTabPicker::listChromeTabs() const
{
    if (!automation_)
        return failure("UI Automation is not initialized.");

    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation_->get_ControlViewWalker(walker.GetAddressOf())) || !walker)
        return failure("Could not obtain the UIA Control View walker.");

    const QJsonArray windows = ChromeWindowEnumerator::enumerateVisibleChromeWindows();

    QJsonArray tabs;
    QStringList errors;
    int browserWindows = 0;

    for (const QJsonValue& value : windows)
    {
        const QJsonObject window = value.toObject();

        const QString title = window.value("title").toString();
        if (window.value("className").toString() != "Chrome_WidgetWin_1"
            || title.isEmpty()
            || title.contains("DevTools", Qt::CaseInsensitive))
            continue;

        bool ok = false;
        const qulonglong handle = window.value("hwnd").toString().toULongLong(&ok, 16);
        if (!ok)
            continue;

        ++browserWindows;
        const QJsonObject windowResult = readWindowTabs(
            reinterpret_cast<HWND>(static_cast<quintptr>(handle)),
            window,
            walker.Get());

        if (!windowResult.value("ok").toBool())
        {
            errors.append(windowResult.value("error").toString());
            continue;
        }

        const QJsonArray windowTabs = windowResult.value("tabs").toArray();
        for (const QJsonValue& tab : windowTabs)
            tabs.append(tab);
    }

    if (browserWindows == 0)
        return failure("No Google Chrome window is open.");

    if (tabs.isEmpty())
    {
        const QString detail = errors.isEmpty()
            ? QString("No Google Chrome browser tabs could be read.")
            : errors.join(" ");
        return failure(detail);
    }

    return QJsonObject{
        {"ok", true},
        {"tabs", tabs},
        {"windowCount", browserWindows}
    };
}

QJsonObject ActiveTabPicker::readWindowTabs(
    HWND hwnd,
    const QJsonObject& window,
    IUIAutomationTreeWalker* walker) const
{
    if (!IsWindow(hwnd))
        return failure("A Chrome window closed while its tabs were being read.");

    ComPtr<IUIAutomationElement> root;
    const HRESULT hr = automation_->ElementFromHandle(hwnd, root.GetAddressOf());
    if (FAILED(hr) || !root)
    {
        return failure(QString("ElementFromHandle failed: 0x%1")
            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
    }

    const BrowserUi ui = scanBrowserUi(walker, root.Get());
    const QString windowTitle = window.value("title").toString();

    QString windowUrlSource;
    const QUrl windowUrl = readWindowAddress(ui, &windowUrlSource);

    int selectedIndex = -1;
    for (int index = 0; index < static_cast<int>(ui.tabItems.size()); ++index)
    {
        if (readBooleanProperty(
                ui.tabItems[static_cast<std::size_t>(index)].Get(),
                UIA_SelectionItemIsSelectedPropertyId))
        {
            selectedIndex = index;
            break;
        }
    }

    if (selectedIndex < 0 && !ui.tabItems.empty())
    {
        // Chrome titles its window "<shown tab title> - Google Chrome".
        // This only labels the shown tab so its address can be attached.
        const int suffix = windowTitle.lastIndexOf(" - ");
        const QString shownTitle = suffix > 0 ? windowTitle.left(suffix) : windowTitle;

        for (int index = 0; index < static_cast<int>(ui.tabItems.size()); ++index)
        {
            const QString name = readStringProperty(
                ui.tabItems[static_cast<std::size_t>(index)].Get(),
                UIA_NamePropertyId);
            if (name.startsWith(shownTitle))
            {
                selectedIndex = index;
                break;
            }
        }
    }

    QJsonArray tabs;

    // Windows without a tab strip are apps, popups or other Chromium UI, not
    // browser tabs.
    for (int index = 0; index < static_cast<int>(ui.tabItems.size()); ++index)
    {
        tabs.append(makeTabRecord(
            ui.tabItems[static_cast<std::size_t>(index)].Get(),
            window,
            index == selectedIndex,
            windowUrl,
            windowUrlSource,
            index));
    }

    return QJsonObject{
        {"ok", true},
        {"tabs", tabs}
    };
}
