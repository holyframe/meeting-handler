#include "UiaWorker.h"

#include "ActiveTabPicker.h"
#include "ChromeWindowEnumerator.h"
#include "UiaTreeInspector.h"
#include "ZoomMeetingController.h"

#include <QJsonObject>

#include <objbase.h>
#include <UIAutomation.h>

using Microsoft::WRL::ComPtr;

UiaWorker::UiaWorker(QObject* parent)
    : QObject(parent)
{
}

UiaWorker::~UiaWorker()
{
    automation2_.Reset();
    automation_.Reset();

    if (comInitialized_)
    {
        CoUninitialize();
        comInitialized_ = false;
    }
}

void UiaWorker::initialize()
{
    if (automation_)
        return;

    if (!comInitialized_)
    {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (FAILED(comHr))
        {
            emit initialized(false,
                QString("CoInitializeEx failed: 0x%1")
                    .arg(static_cast<quint32>(comHr), 8, 16, QLatin1Char('0')));
            return;
        }

        comInitialized_ = true;
    }

    const HRESULT createHr = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(automation_.GetAddressOf()));

    if (FAILED(createHr) || !automation_)
    {
        emit initialized(false,
            QString("Could not create CUIAutomation: 0x%1")
                .arg(static_cast<quint32>(createHr), 8, 16, QLatin1Char('0')));
        return;
    }

    if (SUCCEEDED(automation_.As(&automation2_)) && automation2_)
    {
        // Bound provider wait time so a stale/hung Chrome window is less likely
        // to stall the controller for the full default timeout.
        automation2_->put_ConnectionTimeout(2000);
        automation2_->put_TransactionTimeout(3000);
    }

    emit initialized(true, "Windows UI Automation initialized.");
}

void UiaWorker::scanChromeWindows()
{
    const QJsonArray windows =
        ChromeWindowEnumerator::enumerateVisibleChromeWindows();

    emit chromeWindowsFound(windows);
}

void UiaWorker::inspectWindow(quintptr hwndValue)
{
    initialize();

    if (!automation_)
    {
        emit inspectionFinished(QJsonObject{
            {"ok", false},
            {"error", "UI Automation could not be initialized."}
        });
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(hwndValue);

    UiaTreeInspector inspector(automation_.Get());
    const QJsonObject result = inspector.inspectWindow(hwnd);

    emit inspectionFinished(result);
}

void UiaWorker::listChromeTabs()
{
    initialize();

    if (!automation_)
    {
        emit chromeTabsListed(QJsonObject{
            {"ok", false},
            {"error", "UI Automation could not be initialized."}
        });
        return;
    }

    ActiveTabPicker picker(automation_.Get());
    emit chromeTabsListed(picker.listChromeTabs());
}

void UiaWorker::activateMeeting(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName,
    const QString& requestKey)
{
    initialize();

    QJsonObject result = automation_
        ? ZoomMeetingController(automation_.Get())
              .activateTab(hwndValue, tabRuntimeId, tabName, true)
        : QJsonObject{{"ok", false}, {"error", "UI Automation could not be initialized."}};

    result.insert("requestKey", requestKey);
    emit meetingActivated(result);
}

void UiaWorker::readMicrophone(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName,
    const QString& requestKey)
{
    initialize();

    QJsonObject result = automation_
        ? ZoomMeetingController(automation_.Get())
              .readMicrophone(hwndValue, tabRuntimeId, tabName)
        : QJsonObject{{"ok", false}, {"error", "UI Automation could not be initialized."}};

    result.insert("requestKey", requestKey);
    emit microphoneRead(result);
}

void UiaWorker::setMicrophoneMuted(
    quintptr hwndValue,
    const QString& tabRuntimeId,
    const QString& tabName,
    bool muted,
    const QString& requestKey)
{
    initialize();

    QJsonObject result = automation_
        ? ZoomMeetingController(automation_.Get())
              .setMicrophoneMuted(hwndValue, tabRuntimeId, tabName, muted)
        : QJsonObject{{"ok", false}, {"error", "UI Automation could not be initialized."}};

    result.insert("requestKey", requestKey);
    result.insert("requestedMuted", muted);
    emit microphoneSet(result);
}

void UiaWorker::readAudioConnection(
    quintptr hwndValue, const QString& tabRuntimeId,
    const QString& tabName, const QString& requestKey)
{
    initialize();
    QJsonObject result = automation_
        ? ZoomMeetingController(automation_.Get(), [this](const QString& message) { emit diagnostic(message); })
              .readAudioConnection(hwndValue, tabRuntimeId, tabName)
        : QJsonObject{{"ok", false}, {"error", "UI Automation could not be initialized."}};
    result.insert("requestKey", requestKey);
    emit audioConnectionRead(result);
}

void UiaWorker::setAudioConnected(
    quintptr hwndValue, const QString& tabRuntimeId,
    const QString& tabName, bool connected, const QString& requestKey)
{
    initialize();
    QJsonObject result = automation_
        ? ZoomMeetingController(automation_.Get(), [this](const QString& message) { emit diagnostic(message); })
              .setAudioConnected(hwndValue, tabRuntimeId, tabName, connected)
        : QJsonObject{{"ok", false}, {"error", "UI Automation could not be initialized."}};
    result.insert("requestKey", requestKey);
    result.insert("requestedConnected", connected);
    emit audioConnectionSet(result);
}

void UiaWorker::prepareImportedMeetings(const QJsonArray& targets)
{
    initialize();

    QJsonArray results;
    if (!automation_)
    {
        emit importedMeetingsPrepared(results);
        return;
    }

    const ZoomMeetingController controller(automation_.Get());

    for (const QJsonValue& value : targets)
    {
        const QJsonObject target = value.toObject();

        bool ok = false;
        const qulonglong handle =
            target.value("windowHandle").toString().toULongLong(&ok, 16);

        QJsonObject result = ok
            ? controller.activateAndReadMicrophone(
                  static_cast<quintptr>(handle),
                  target.value("tabRuntimeId").toString(),
                  target.value("tabName").toString())
            : QJsonObject{{"ok", false}, {"error", "The Chrome window handle is invalid."}};

        result.insert("requestKey", target.value("requestKey"));
        results.append(result);
    }

    emit importedMeetingsPrepared(results);
}

void UiaWorker::shutdown()
{
    automation2_.Reset();
    automation_.Reset();

    if (comInitialized_)
    {
        CoUninitialize();
        comInitialized_ = false;
    }
}
