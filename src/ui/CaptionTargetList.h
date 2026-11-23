#pragma once

#include <QList>
#include <QWidget>

class QFrame;
class QVBoxLayout;
class WebInputPicker;

class CaptionTargetList final : public QWidget {
    Q_OBJECT
public:
    explicit CaptionTargetList(WebInputPicker& picker, QWidget* parent = nullptr);
    void Refresh();
    bool CancelRemoval();

signals:
    void selectionChanged();
    void statusChanged(const QString& message);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QFrame* createTile();
    void placeName(QFrame* tile);
    void closeTile(int index);
    void markActive();
    void updateRemoval(QFrame* tile, const QPoint& screenPoint);

    WebInputPicker& m_picker;
    QVBoxLayout* m_layout;
    QList<QFrame*> m_tiles;
    QFrame* m_pressed = nullptr;
    QFrame* m_removing = nullptr;
    QPoint m_pressPoint;
    bool m_reordered = false;
    bool m_outside = false;
};
