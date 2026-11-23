#pragma once

#include <QWidget>
#include <windows.h>

class WindowPickOutline final : public QWidget {
public:
    explicit WindowPickOutline(QWidget* owner);
    void showAround(HWND target);

private:
    bool setFrameRegion(HWND outline, const QSize& size);
    QSize m_regionSize;
    int m_border = 0;
};
