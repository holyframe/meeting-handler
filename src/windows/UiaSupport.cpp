#include "UiaSupport.h"

#include <QStringList>

#include <OleAuto.h>
#include <deque>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr int kMaximumBrowserUiNodes = 4000;
}

namespace uia
{
QString readStringProperty(IUIAutomationElement* element, PROPERTYID propertyId)
{
    if (!element)
        return {};

    VARIANT variant;
    VariantInit(&variant);

    QString value;
    if (SUCCEEDED(element->GetCurrentPropertyValue(propertyId, &variant))
        && variant.vt == VT_BSTR && variant.bstrVal)
    {
        value = QString::fromWCharArray(
            variant.bstrVal,
            static_cast<qsizetype>(SysStringLen(variant.bstrVal)));
    }

    VariantClear(&variant);
    return value;
}

bool readBooleanProperty(IUIAutomationElement* element, PROPERTYID propertyId)
{
    if (!element)
        return false;

    VARIANT variant;
    VariantInit(&variant);

    const bool value =
        SUCCEEDED(element->GetCurrentPropertyValue(propertyId, &variant))
        && variant.vt == VT_BOOL
        && variant.boolVal == VARIANT_TRUE;

    VariantClear(&variant);
    return value;
}

QString readRuntimeId(IUIAutomationElement* element)
{
    if (!element)
        return {};

    SAFEARRAY* array = nullptr;
    if (FAILED(element->GetRuntimeId(&array)) || !array)
        return {};

    LONG lower = 0;
    LONG upper = -1;
    SafeArrayGetLBound(array, 1, &lower);
    SafeArrayGetUBound(array, 1, &upper);

    QStringList parts;
    for (LONG index = lower; index <= upper; ++index)
    {
        int value = 0;
        if (SUCCEEDED(SafeArrayGetElement(array, &index, &value)))
            parts.append(QString::number(value));
    }

    SafeArrayDestroy(array);
    return parts.join('.');
}

BrowserUi scanBrowserUi(IUIAutomationTreeWalker* walker, IUIAutomationElement* root)
{
    BrowserUi ui;

    if (!walker || !root)
        return ui;

    std::deque<ComPtr<IUIAutomationElement>> pending;
    pending.push_back(root);

    int visited = 0;

    while (!pending.empty() && visited < kMaximumBrowserUiNodes)
    {
        ComPtr<IUIAutomationElement> element = std::move(pending.front());
        pending.pop_front();
        ++visited;

        CONTROLTYPEID type = 0;
        element->get_CurrentControlType(&type);

        if (type == UIA_DocumentControlTypeId)
        {
            ui.documents.push_back(element);
            continue;
        }

        if (type == UIA_TabItemControlTypeId)
            ui.tabItems.push_back(element);
        else if (type == UIA_EditControlTypeId)
            ui.edits.push_back(element);

        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(element.Get(), child.GetAddressOf())))
            continue;

        while (child)
        {
            pending.push_back(child);

            ComPtr<IUIAutomationElement> nextSibling;
            if (FAILED(walker->GetNextSiblingElement(
                    child.Get(), nextSibling.GetAddressOf())))
                break;

            child = std::move(nextSibling);
        }
    }

    return ui;
}
}
