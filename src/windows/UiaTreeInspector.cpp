#include "UiaTreeInspector.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <OleAuto.h>
#include <algorithm>
#include <vector>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
QString bstrToQString(BSTR value)
{
    if (!value)
        return {};

    const QString result = QString::fromWCharArray(
        value,
        static_cast<qsizetype>(SysStringLen(value)));

    SysFreeString(value);
    return result;
}

QString controlTypeName(CONTROLTYPEID type)
{
    switch (type)
    {
    case UIA_ButtonControlTypeId: return "Button";
    case UIA_CheckBoxControlTypeId: return "CheckBox";
    case UIA_ComboBoxControlTypeId: return "ComboBox";
    case UIA_DocumentControlTypeId: return "Document";
    case UIA_EditControlTypeId: return "Edit";
    case UIA_GroupControlTypeId: return "Group";
    case UIA_HyperlinkControlTypeId: return "Hyperlink";
    case UIA_ListControlTypeId: return "List";
    case UIA_ListItemControlTypeId: return "ListItem";
    case UIA_MenuControlTypeId: return "Menu";
    case UIA_MenuItemControlTypeId: return "MenuItem";
    case UIA_PaneControlTypeId: return "Pane";
    case UIA_RadioButtonControlTypeId: return "RadioButton";
    case UIA_ScrollBarControlTypeId: return "ScrollBar";
    case UIA_TabControlTypeId: return "Tab";
    case UIA_TabItemControlTypeId: return "TabItem";
    case UIA_TextControlTypeId: return "Text";
    case UIA_ToolBarControlTypeId: return "ToolBar";
    case UIA_TreeControlTypeId: return "Tree";
    case UIA_TreeItemControlTypeId: return "TreeItem";
    case UIA_WindowControlTypeId: return "Window";
    default: return QString("ControlType(%1)").arg(type);
    }
}

bool readBooleanProperty(
    IUIAutomationElement* element,
    PROPERTYID propertyId,
    bool* value)
{
    if (!element || !value)
        return false;

    VARIANT variant;
    VariantInit(&variant);

    const HRESULT hr = element->GetCurrentPropertyValue(propertyId, &variant);
    if (SUCCEEDED(hr) && variant.vt == VT_BOOL)
    {
        *value = (variant.boolVal == VARIANT_TRUE);
        VariantClear(&variant);
        return true;
    }

    VariantClear(&variant);
    return false;
}

struct PendingNode
{
    ComPtr<IUIAutomationElement> element;
    int depth = 0;
};
}

UiaTreeInspector::UiaTreeInspector(IUIAutomation* automation)
{
    automation_ = automation;
}

QJsonObject UiaTreeInspector::inspectWindow(
    HWND hwnd,
    int maximumNodes,
    int maximumDepth) const
{
    QJsonObject result;

    if (!automation_)
    {
        result.insert("ok", false);
        result.insert("error", "UI Automation is not initialized.");
        return result;
    }

    if (!IsWindow(hwnd))
    {
        result.insert("ok", false);
        result.insert("error", "The selected Chrome window no longer exists.");
        return result;
    }

    ComPtr<IUIAutomationElement> root;
    HRESULT hr = automation_->ElementFromHandle(hwnd, root.GetAddressOf());

    if (FAILED(hr) || !root)
    {
        result.insert("ok", false);
        result.insert("error", QString("ElementFromHandle failed: 0x%1")
            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        return result;
    }

    ComPtr<IUIAutomationTreeWalker> walker;
    hr = automation_->get_ControlViewWalker(walker.GetAddressOf());

    if (FAILED(hr) || !walker)
    {
        result.insert("ok", false);
        result.insert("error", "Could not obtain the UIA Control View walker.");
        return result;
    }

    QJsonArray elements;
    std::vector<PendingNode> pending;
    pending.push_back({root, 0});

    int visited = 0;

    while (!pending.empty() && visited < maximumNodes)
    {
        PendingNode node = std::move(pending.back());
        pending.pop_back();

        if (!node.element)
            continue;

        ++visited;

        BSTR nameBstr = nullptr;
        BSTR automationIdBstr = nullptr;
        BSTR classNameBstr = nullptr;

        QString name;
        QString automationId;
        QString className;

        if (SUCCEEDED(node.element->get_CurrentName(&nameBstr)))
            name = bstrToQString(nameBstr);
        else if (nameBstr)
            SysFreeString(nameBstr);

        if (SUCCEEDED(node.element->get_CurrentAutomationId(&automationIdBstr)))
            automationId = bstrToQString(automationIdBstr);
        else if (automationIdBstr)
            SysFreeString(automationIdBstr);

        if (SUCCEEDED(node.element->get_CurrentClassName(&classNameBstr)))
            className = bstrToQString(classNameBstr);
        else if (classNameBstr)
            SysFreeString(classNameBstr);

        CONTROLTYPEID controlType = 0;
        node.element->get_CurrentControlType(&controlType);

        BOOL isOffscreen = FALSE;
        BOOL isEnabled = FALSE;
        node.element->get_CurrentIsOffscreen(&isOffscreen);
        node.element->get_CurrentIsEnabled(&isEnabled);

        bool invokeAvailable = false;
        bool toggleAvailable = false;
        bool selectionAvailable = false;

        readBooleanProperty(
            node.element.Get(),
            UIA_IsInvokePatternAvailablePropertyId,
            &invokeAvailable);

        readBooleanProperty(
            node.element.Get(),
            UIA_IsTogglePatternAvailablePropertyId,
            &toggleAvailable);

        readBooleanProperty(
            node.element.Get(),
            UIA_IsSelectionItemPatternAvailablePropertyId,
            &selectionAvailable);

        RECT bounds{};
        const HRESULT boundsHr =
            node.element->get_CurrentBoundingRectangle(&bounds);

        QJsonObject item;
        item.insert("depth", node.depth);
        item.insert("controlTypeId", static_cast<int>(controlType));
        item.insert("controlType", controlTypeName(controlType));
        item.insert("name", name);
        item.insert("automationId", automationId);
        item.insert("className", className);
        item.insert("enabled", isEnabled == TRUE);
        item.insert("offscreen", isOffscreen == TRUE);
        item.insert("invokePattern", invokeAvailable);
        item.insert("togglePattern", toggleAvailable);
        item.insert("selectionItemPattern", selectionAvailable);

        if (SUCCEEDED(boundsHr))
        {
            QJsonObject rect;
            rect.insert("left", static_cast<int>(bounds.left));
            rect.insert("top", static_cast<int>(bounds.top));
            rect.insert("right", static_cast<int>(bounds.right));
            rect.insert("bottom", static_cast<int>(bounds.bottom));
            item.insert("bounds", rect);
        }

        elements.append(item);

        if (node.depth >= maximumDepth)
            continue;

        ComPtr<IUIAutomationElement> child;
        hr = walker->GetFirstChildElement(
            node.element.Get(),
            child.GetAddressOf());

        if (FAILED(hr) || !child)
            continue;

        std::vector<ComPtr<IUIAutomationElement>> children;

        while (child && children.size() < static_cast<std::size_t>(maximumNodes))
        {
            children.push_back(child);

            ComPtr<IUIAutomationElement> nextSibling;
            hr = walker->GetNextSiblingElement(
                child.Get(),
                nextSibling.GetAddressOf());

            if (FAILED(hr))
                break;

            child = std::move(nextSibling);
        }

        // Push in reverse so the traversal output preserves tree order.
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            pending.push_back({*it, node.depth + 1});
    }

    result.insert("ok", true);
    result.insert("windowHandle", QString::number(
        static_cast<qulonglong>(reinterpret_cast<quintptr>(hwnd)), 16));
    result.insert("visitedNodes", visited);
    result.insert("truncated", !pending.empty());
    result.insert("elements", elements);

    return result;
}
