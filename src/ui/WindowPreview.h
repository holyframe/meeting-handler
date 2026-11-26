#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <QWidget>
#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QMouseEvent;
class QWheelEvent;

class WindowPreview final : public QWidget {
public:
    WindowPreview(HWND source, std::function<bool()> sourceExists);
    ~WindowPreview() override;
    bool open();
    void refresh();
    void beginCrop();
    void setInteractive(bool enabled);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateCursor();
    QRectF croppedArea(const QRectF& selection) const;
    bool mapToSource(const QPointF& position, POINT& point) const;
    bool forwardMouse(QMouseEvent* event);
    bool forwardWheel(QWheelEvent* event);
    void releaseButtons();
    HWND m_source;
    std::function<bool()> m_sourceExists;
    HTHUMBNAIL m_thumbnail = nullptr;
    QLabel* m_canvas;
    QPushButton* m_cropButton;
    QPushButton* m_reset;
    bool m_interactive = true;
    bool m_live = false;
    QRectF m_crop{0, 0, 1, 1}; // Source fractions survive source size and DPI changes.
    QRectF m_view;              // Displayed image in canvas coordinates, excluding letterboxing.
    std::optional<QPointF> m_dragStart;
    QPointF m_dragEnd;
    HWND m_pressedWindow = nullptr;
    Qt::MouseButtons m_pressedButtons;
    LPARAM m_lastMousePoint = 0;
    RECT m_sourceFrame{};
    RECT m_lastSource{}, m_lastDestination{};
};
