#pragma once

#include <QAbstractScrollArea>
#include <QTextLayout>
#include <QTimer>

#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QToolButton;

// Keeps absolute transcript positions so a selection survives rewrites and front trimming.
class CaptionView final : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit CaptionView(QWidget* parent = nullptr);
    void QueueUpdate(size_t firstDirtyLine, std::vector<std::wstring> lines);
    void Present();
    void Clear();
    void SetTypography(const std::wstring& family, int pointSize, double lineSpacing);
    std::wstring FullText() const;
    bool Empty() const { return m_lines.empty(); }
    bool HasSelection() const;
    std::wstring SelectedText() const;
    void SelectAll();
    void ClearSelection();

signals:
    void copyRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int, int) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    struct Line {
        std::wstring text;
        std::unique_ptr<QTextLayout> layout;
        qreal height = -1;
    };
    struct TextPos {
        size_t line = 0;
        size_t offset = 0;
        auto operator<=>(const TextPos&) const = default;
    };
    enum class DragKind { None, Text, Line };

    QTextLayout& layoutFor(size_t index);
    qreal heightFor(size_t index);
    qreal rowHeight() const;
    void invalidateLayouts();
    void refreshScrollRange();
    void updateJumpButton();
    size_t lineAt(qreal y, qreal* top = nullptr);
    TextPos positionAt(QPoint point, bool* insideText = nullptr);
    void updateDrag(QPoint point);
    void autoScroll();
    TextPos TranscriptEnd() const;
    TextPos LineEndExclusive(size_t absoluteLine) const;
    bool  FarEndAtTranscriptEnd() const;
    // Auto-follow the bottom unless a mid-transcript selection would scroll away.
    bool  ShouldFollowBottom() const;
    void  OrderedSelection(TextPos* start, TextPos* end) const;
    void  ResetSelection();
    void  ClampSelection();
    void  GrowSelectionEndIfNeeded(bool wasAtEnd);
    void  MoveSelectionTo(const TextPos& caret, bool keepAnchor);
    void  SelectWordAt(const TextPos& position);
    void  SelectLineThroughEnd(size_t absoluteLine);
    void  SelectLinesTo(size_t absoluteLine, bool keepAnchor);

    std::vector<Line> m_lines;
    size_t m_lineOffset = 0;
    static constexpr size_t kMaxLines = 3000;
    int m_measuredWidth = -1;
    qreal m_totalHeight = -1;
    QFont m_textFont;
    double m_lineSpacing = 1.3;
    bool m_stickToBottom = true;
    bool m_forceFollow = false;
    bool m_updatingRange = false;

    TextPos  m_selectionAnchor;
    TextPos  m_selectionCaret;
    bool     m_selectionValid = false;
    DragKind m_dragKind = DragKind::None;
    size_t   m_lineDragOrigin = 0;
    bool     m_lineSelectionActive = false;
    QPoint m_dragPoint;
    std::optional<size_t> m_hoverLine;
    QTimer m_autoScroll;
    QToolButton* m_jumpButton;
};
