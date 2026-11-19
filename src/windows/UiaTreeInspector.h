#pragma once

#include <QJsonObject>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

class UiaTreeInspector
{
public:
    explicit UiaTreeInspector(IUIAutomation* automation);

    QJsonObject inspectWindow(HWND hwnd,
                              int maximumNodes = 1200,
                              int maximumDepth = 12) const;

private:
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
};
