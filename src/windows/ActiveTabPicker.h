#pragma once

#include <QJsonObject>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

class QUrl;

class ActiveTabPicker
{
public:
    explicit ActiveTabPicker(IUIAutomation* automation);

    QJsonObject listChromeTabs() const;
    static bool isZoomMeetingTab(const QUrl& address, const QString& tabTitle, QString* meetingId = nullptr);

private:
    QJsonObject readWindowTabs(HWND hwnd,
                               const QJsonObject& window,
                               IUIAutomationTreeWalker* walker) const;

    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
};
