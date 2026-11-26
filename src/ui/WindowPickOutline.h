#pragma once

#include <QWidget>
#include <windows.h>

class WindowPickOutline final : public QWidget {
public:
    explicit WindowPickOutline(QWidget* owner);
    void setAppearance(const QColor& color, int widthHundredths);
    void showAround(HWND target, bool followTargetStack = false);

private:
    bool setFrameRegion(HWND outline, const QSize& size);
    QSize m_regionSize;
    int m_border = 0;
    int m_widthHundredths = 300;
};
