#pragma once

#include <QJsonObject>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

class ActiveTabPicker
{
public:
    explicit ActiveTabPicker(IUIAutomation* automation);

    QJsonObject listChromeTabs() const;

private:
    QJsonObject readWindowTabs(HWND hwnd,
                               const QJsonObject& window,
                               IUIAutomationTreeWalker* walker) const;

    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
};
