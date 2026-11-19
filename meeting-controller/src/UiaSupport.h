#pragma once

#include <QString>

#include <vector>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace uia
{
QString readStringProperty(IUIAutomationElement* element, PROPERTYID propertyId);
bool readBooleanProperty(IUIAutomationElement* element, PROPERTYID propertyId);
QString readRuntimeId(IUIAutomationElement* element);

// Chrome's own controls, with page content left unexplored: the Zoom page has
// its own tabs and text fields that must not be mistaken for the browser's.
struct BrowserUi
{
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> tabItems;
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> edits;
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> documents;
};

BrowserUi scanBrowserUi(IUIAutomationTreeWalker* walker, IUIAutomationElement* root);
}
