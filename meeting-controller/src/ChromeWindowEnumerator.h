#pragma once

#include <QJsonArray>
#include <QString>

class ChromeWindowEnumerator
{
public:
    static QJsonArray enumerateVisibleChromeWindows();
    static QString windowHandleToString(quintptr hwnd);
};
