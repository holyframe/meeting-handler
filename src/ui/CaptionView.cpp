#include "CaptionView.h"

#include <QApplication>
#include <QFont>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace {
constexpr int gutter = 120;
constexpr int rightMargin = 14;
constexpr int topMargin = 10;

bool IsWordCharacter(wchar_t ch) {
    return std::iswalnum(static_cast<wint_t>(ch)) != 0 || ch == L'_';
}
}

CaptionView::CaptionView(QWidget* parent)
    : QAbstractScrollArea(parent), m_textFont("Segoe UI", 12), m_jumpButton(new QToolButton(viewport())) {
    setObjectName("captionView");
    setAccessibleName(tr("Live caption transcript"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAutoFillBackground(false);
    m_jumpButton->setObjectName("jumpToLatest");
    m_jumpButton->setAutoRaise(true);
    m_jumpButton->setText(QChar(0xE70D));
    m_jumpButton->setAccessibleName(tr("Jump to latest caption"));
    m_jumpButton->setToolTip(m_jumpButton->accessibleName());
    m_jumpButton->setFixedSize(32, 32);
    m_jumpButton->hide();
    connect(m_jumpButton, &QToolButton::clicked, this, [this] {
        m_forceFollow = true;
        m_stickToBottom = true;
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_updatingRange) {
            m_stickToBottom = value >= verticalScrollBar()->maximum() - 1;
            if (!m_stickToBottom) m_forceFollow = false;
        }
        updateJumpButton();
    });
    m_autoScroll.setInterval(40);
    connect(&m_autoScroll, &QTimer::timeout, this, &CaptionView::autoScroll);
}

qreal CaptionView::rowHeight() const {
    return m_textFont.pointSizeF() * viewport()->logicalDpiY() / 72.0 * 1.2 * m_lineSpacing;
}

QTextLayout& CaptionView::layoutFor(size_t index) {
    auto& line = m_lines[index];
    if (line.layout) return *line.layout;
    line.layout = std::make_unique<QTextLayout>(QString::fromStdWString(line.text), m_textFont, viewport());
    line.layout->setCacheEnabled(true);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    line.layout->setTextOption(option);
    const qreal width = std::max(32, viewport()->width() - gutter - rightMargin);
    const qreal step = rowHeight();
    qreal y = 0;
    line.layout->beginLayout();
    for (;;) {
        auto row = line.layout->createLine();
        if (!row.isValid()) break;
        row.setLineWidth(width);
        row.setPosition(QPointF(0, y + (step - row.height()) / 2));
        y += step;
    }
    line.layout->endLayout();
    line.height = std::max(y, step);
    return *line.layout;
}

qreal CaptionView::heightFor(size_t index) {
    if (m_lines[index].height < 0) layoutFor(index);
    return m_lines[index].height;
}

void CaptionView::invalidateLayouts() {
    for (auto& line : m_lines) {
        line.layout.reset();
        line.height = -1;
    }
    m_totalHeight = -1;
}

void CaptionView::refreshScrollRange() {
    if (viewport()->width() != m_measuredWidth) {
        m_measuredWidth = viewport()->width();
        invalidateLayouts();
    }
    if (m_totalHeight < 0) {
        m_totalHeight = 2 * topMargin;
        for (size_t i = 0; i < m_lines.size(); ++i) m_totalHeight += heightFor(i);
    }
    auto* bar = verticalScrollBar();
    const bool follow = ShouldFollowBottom();
    QScopedValueRollback guard(m_updatingRange, true);
    bar->setPageStep(viewport()->height());
    bar->setSingleStep(std::max(1, qRound(m_textFont.pointSizeF() * 1.6)));
    bar->setRange(0, std::max(0, qCeil(m_totalHeight) - viewport()->height()));
    if (follow) bar->setValue(bar->maximum());
    m_stickToBottom = bar->value() >= bar->maximum() - 1;
    if (!m_stickToBottom) m_forceFollow = false;
    updateJumpButton();
    viewport()->update();
}

void CaptionView::updateJumpButton() {
    const auto* bar = verticalScrollBar();
    m_jumpButton->move((viewport()->width() - 32) / 2, viewport()->height() - 48);
    m_jumpButton->setVisible(viewport()->height() >= 56 && bar->maximum() - bar->value() > 8);
}

void CaptionView::QueueUpdate(size_t firstDirtyLine, std::vector<std::wstring> lines) {
    const bool growEnd = FarEndAtTranscriptEnd();

    // Translate the transcript-absolute index into our trimmed window.
    size_t local = 0;
    const size_t heldEnd = m_lineOffset + m_lines.size();
    if (m_lines.empty() || firstDirtyLine < m_lineOffset || firstDirtyLine > heldEnd) {
        // Clear() deliberately forgets the absolute index of the text it
        // removed. The first later update may therefore begin at line 40 (for
        // example), rather than zero. Rebase the empty model onto that absolute
        // index. The same rule repairs a genuine gap caused by missed updates.
        //
        // Clamping a gapped index to m_lines.size() looks harmless, but after a
        // clear it turns every rewrite of the same live sentence into another
        // appended line: "Because", "Because believing", ...
        m_lines.clear();
        m_lineOffset = firstDirtyLine;
        local = 0;
    } else {
        local = firstDirtyLine - m_lineOffset;
    }

    m_lines.resize(local);
    m_lines.reserve(local + lines.size());
    for (std::wstring& text : lines) {
        Line line;
        line.text = std::move(text);
        m_lines.push_back(std::move(line));
    }

    if (m_lines.size() > kMaxLines) {
        const size_t drop = m_lines.size() - kMaxLines;
        m_lines.erase(m_lines.begin(), m_lines.begin() + static_cast<ptrdiff_t>(drop));
        m_lineOffset += drop;
    }

    m_totalHeight = -1.0f;
    // A revised or trimmed line can leave an endpoint pointing past the end of
    // its text; pull it back in rather than dropping the selection outright, so
    // a selection made while the speaker is still talking survives.
    ClampSelection();
    GrowSelectionEndIfNeeded(growEnd);
}

void CaptionView::Present() {
    refreshScrollRange();
    viewport()->repaint();
}

void CaptionView::Clear() {
    m_autoScroll.stop();
    m_dragKind = DragKind::None;
    m_lines.clear();
    m_lineOffset = 0;
    m_totalHeight = -1;
    m_stickToBottom = true;
    m_forceFollow = false;
    m_hoverLine.reset();
    ResetSelection();
    refreshScrollRange();
}

void CaptionView::SetTypography(const std::wstring& family, int pointSize, double spacing) {
    m_textFont = QFont(QString::fromStdWString(family), pointSize);
    m_lineSpacing = spacing;
    invalidateLayouts();
    refreshScrollRange();
}

void CaptionView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    refreshScrollRange();
}

void CaptionView::scrollContentsBy(int, int) {
    viewport()->update();
}

void CaptionView::paintEvent(QPaintEvent*) {
    QPainter painter(viewport());
    const auto colors = palette();
    painter.fillRect(viewport()->rect(), colors.base());
    painter.setPen(colors.text().color());
    TextPos start, end;
    OrderedSelection(&start, &end);
    qreal y = topMargin - verticalScrollBar()->value();
    for (size_t i = 0; i < m_lines.size(); ++i) {
        auto& item = m_lines[i];
        const qreal height = heightFor(i);
        if (y + height < 0 || y > viewport()->height()) {
            item.layout.reset();  // retain the measured height, discard offscreen shaping
        } else {
            const size_t absolute = m_lineOffset + i;
            if (m_hoverLine == absolute)
                painter.fillRect(QRectF(0, y, viewport()->width(), height), colors.alternateBase());
            auto& layout = layoutFor(i);
            QList<QTextLayout::FormatRange> selection;
            if (HasSelection() && start.line <= absolute && absolute <= end.line) {
                const auto from = absolute == start.line ? start.offset : 0;
                const auto to = absolute == end.line ? end.offset : item.text.size();
                QTextLayout::FormatRange range;
                range.start = static_cast<int>(from);
                range.length = static_cast<int>(to - from);
                range.format.setBackground(colors.highlight());
                range.format.setForeground(colors.highlightedText());
                selection.append(range);
                if (absolute < end.line && layout.lineCount()) {
                    const auto last = layout.lineAt(layout.lineCount() - 1);
                    const qreal x = last.cursorToX(static_cast<int>(item.text.size()));
                    painter.fillRect(QRectF(gutter + x, y + height - rowHeight(),
                                            m_textFont.pointSizeF() * .45, rowHeight()), colors.highlight());
                }
            }
            layout.draw(&painter, QPointF(gutter, y), selection);
        }
        y += height;
    }
}

size_t CaptionView::lineAt(qreal y, qreal* top) {
    qreal origin = topMargin - verticalScrollBar()->value();
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const qreal height = heightFor(i);
        if (y < origin + height || i + 1 == m_lines.size()) {
            if (top) *top = origin;
            return i;
        }
        origin += height;
    }
    return 0;
}

CaptionView::TextPos CaptionView::positionAt(QPoint point, bool* insideText) {
    qreal top = 0;
    const size_t index = lineAt(point.y(), &top);
    const auto& item = m_lines[index];
    auto& layout = layoutFor(index);
    const qreal y = point.y() - top;
    if (insideText) *insideText = false;
    if (y >= item.height || layout.lineCount() == 0) return {m_lineOffset + index, item.text.size()};
    if (y < 0) return {m_lineOffset + index, 0};
    const auto row = layout.lineAt(std::clamp(int(y / rowHeight()), 0, layout.lineCount() - 1));
    const int x = point.x() - gutter;
    if (insideText) *insideText = x >= row.naturalTextRect().left() && x < row.naturalTextRect().right();
    return {m_lineOffset + index, static_cast<size_t>(row.xToCursor(x))};
}

CaptionView::TextPos CaptionView::TranscriptEnd() const {
    if (m_lines.empty()) return {};
    return TextPos{m_lineOffset + m_lines.size() - 1, m_lines.back().text.size()};
}

CaptionView::TextPos CaptionView::LineEndExclusive(size_t absoluteLine) const {
    if (m_lines.empty()) return {};
    const size_t last = m_lineOffset + m_lines.size() - 1;
    if (absoluteLine >= last) {
        return TextPos{last, m_lines.back().text.size()};
    }
    return TextPos{absoluteLine + 1, 0};
}

bool CaptionView::FarEndAtTranscriptEnd() const {
    if (!HasSelection() || m_lines.empty()) return false;
    TextPos start;
    TextPos end;
    OrderedSelection(&start, &end);
    return end == TranscriptEnd();
}

bool CaptionView::ShouldFollowBottom() const {
    if (!m_stickToBottom) return false;
    // The scroll-to-bottom button is an explicit request to catch up and stay
    // with new captions, even when a selection had pinned the viewport.
    if (m_forceFollow) return true;
    // A selection that is not growing with the transcript must keep its
    // viewport — following the bottom would scroll the highlight out of sight
    // when the pane is resized or new captions arrive while the user is away.
    if (HasSelection() && !FarEndAtTranscriptEnd()) return false;
    return true;
}

bool CaptionView::HasSelection() const {
    return m_selectionValid && !(m_selectionAnchor == m_selectionCaret);
}

void CaptionView::OrderedSelection(TextPos* start, TextPos* end) const {
    const bool forward = !(m_selectionCaret < m_selectionAnchor);
    *start = forward ? m_selectionAnchor : m_selectionCaret;
    *end = forward ? m_selectionCaret : m_selectionAnchor;
}

void CaptionView::ResetSelection() {
    m_selectionValid = false;
    m_lineSelectionActive = false;
    m_selectionAnchor = TextPos{};
    m_selectionCaret = TextPos{};
}

void CaptionView::ClearSelection() {
    const bool had = HasSelection();
    ResetSelection();
    if (had) viewport()->update();
}

void CaptionView::ClampSelection() {
    if (!m_selectionValid) return;
    if (m_lines.empty()) {
        ResetSelection();
        return;
    }

    const size_t lastLine = m_lineOffset + m_lines.size() - 1;
    const auto clamp = [&](TextPos& position) {
        if (position.line < m_lineOffset) {
            position.line = m_lineOffset;
            position.offset = 0;
        } else if (position.line > lastLine) {
            position.line = lastLine;
            position.offset = m_lines.back().text.size();
        }
        const std::wstring& text = m_lines[position.line - m_lineOffset].text;
        position.offset = std::min(position.offset, text.size());
    };
    clamp(m_selectionAnchor);
    clamp(m_selectionCaret);

    if (m_selectionAnchor == m_selectionCaret) ResetSelection();
}

void CaptionView::GrowSelectionEndIfNeeded(bool wasAtEnd) {
    if (!wasAtEnd || !m_selectionValid || m_lines.empty()) return;

    const TextPos newEnd = TranscriptEnd();
    if (m_selectionCaret < m_selectionAnchor) {
        m_selectionAnchor = newEnd;
    } else {
        m_selectionCaret = newEnd;
    }
}

void CaptionView::MoveSelectionTo(const TextPos& caret, bool keepAnchor) {
    const TextPos previousAnchor = m_selectionAnchor;
    const TextPos previousCaret = m_selectionCaret;
    const bool wasValid = m_selectionValid;

    if (!keepAnchor || !m_selectionValid) m_selectionAnchor = caret;
    m_selectionCaret = caret;
    m_selectionValid = true;
    m_lineSelectionActive = false;

    // Mouse moves arrive far more often than the selection actually changes.
    if (wasValid && previousAnchor == m_selectionAnchor && previousCaret == m_selectionCaret) return;
    viewport()->update();
}

void CaptionView::SelectWordAt(const TextPos& position) {
    if (position.line < m_lineOffset) return;
    const size_t index = position.line - m_lineOffset;
    if (index >= m_lines.size()) return;

    const std::wstring& text = m_lines[index].text;
    if (text.empty()) return;

    size_t start = std::min(position.offset, text.size() - 1);
    size_t end = start;
    if (IsWordCharacter(text[start])) {
        while (start > 0 && IsWordCharacter(text[start - 1])) --start;
        while (end < text.size() && IsWordCharacter(text[end])) ++end;
    } else {
        end = start + 1;  // punctuation or space: take just that character
    }

    m_selectionAnchor = TextPos{position.line, start};
    m_selectionCaret = TextPos{position.line, end};
    m_selectionValid = true;
    m_lineSelectionActive = false;
    viewport()->update();
}

void CaptionView::SelectLineThroughEnd(size_t absoluteLine) {
    if (m_lines.empty()) return;

    const size_t first = m_lineOffset;
    const size_t last = m_lineOffset + m_lines.size() - 1;
    absoluteLine = std::clamp(absoluteLine, first, last);

    m_lineDragOrigin = absoluteLine;
    m_selectionAnchor = TextPos{absoluteLine, 0};
    m_selectionCaret = TranscriptEnd();
    m_selectionValid = true;
    m_lineSelectionActive = true;
    viewport()->update();
}

void CaptionView::SelectLinesTo(size_t absoluteLine, bool keepAnchor) {
    if (m_lines.empty()) return;

    const size_t first = m_lineOffset;
    const size_t last = m_lineOffset + m_lines.size() - 1;
    absoluteLine = std::clamp(absoluteLine, first, last);

    if (!keepAnchor || !m_selectionValid) {
        m_lineDragOrigin = absoluteLine;
        m_selectionAnchor = TextPos{absoluteLine, 0};
        m_selectionCaret = LineEndExclusive(absoluteLine);
        m_selectionValid = true;
        m_lineSelectionActive = true;
        viewport()->update();
        return;
    }

    // Dragging up past the origin flips the anchor to the end of that origin
    // row so the row the gesture started on stays wholly selected — same as
    // VS Code's line-number gutter.
    if (absoluteLine >= m_lineDragOrigin) {
        m_selectionAnchor = TextPos{m_lineDragOrigin, 0};
        m_selectionCaret = LineEndExclusive(absoluteLine);
    } else {
        m_selectionAnchor = LineEndExclusive(m_lineDragOrigin);
        m_selectionCaret = TextPos{absoluteLine, 0};
    }
    m_selectionValid = true;
    m_lineSelectionActive = true;
    viewport()->update();
}

void CaptionView::SelectAll() {
    if (m_lines.empty()) return;
    m_selectionAnchor = TextPos{m_lineOffset, 0};
    m_selectionCaret = TextPos{m_lineOffset + m_lines.size() - 1, m_lines.back().text.size()};
    m_selectionValid = true;
    m_lineSelectionActive = true;
    m_lineDragOrigin = m_lineOffset;
    viewport()->update();
}

std::wstring CaptionView::SelectedText() const {
    if (!HasSelection()) return {};

    TextPos start;
    TextPos end;
    OrderedSelection(&start, &end);

    std::wstring out;
    for (size_t absolute = std::max(start.line, m_lineOffset); absolute <= end.line; ++absolute) {
        const size_t index = absolute - m_lineOffset;
        if (index >= m_lines.size()) break;

        const std::wstring& text = m_lines[index].text;
        const size_t from = absolute == start.line ? std::min(start.offset, text.size()) : 0;
        const size_t to = absolute == end.line ? std::min(end.offset, text.size()) : text.size();
        if (to > from) out.append(text, from, to - from);
        if (absolute < end.line) out += L"\r\n";
    }
    return out;
}

std::wstring CaptionView::FullText() const {
    size_t total = 0;
    for (const Line& line : m_lines) total += line.text.size() + 2;

    std::wstring out;
    out.reserve(total);
    for (const Line& line : m_lines) {
        out += line.text;
        out += L"\r\n";
    }
    return out;
}

void CaptionView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return QAbstractScrollArea::mousePressEvent(event);
    setFocus(Qt::MouseFocusReason);
    if (m_lines.empty()) return;
    m_dragPoint = event->position().toPoint();
    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier) && m_selectionValid;
    if (m_dragPoint.x() < gutter) {
        m_dragKind = DragKind::Line;
        const size_t absolute = m_lineOffset + lineAt(m_dragPoint.y());
        if (extend) {
            if (!m_lineSelectionActive) m_lineDragOrigin = m_selectionAnchor.line;
            SelectLinesTo(absolute, true);
        } else {
            SelectLineThroughEnd(absolute);
        }
    } else {
        m_dragKind = DragKind::Text;
        MoveSelectionTo(positionAt(m_dragPoint), extend);
    }
}

void CaptionView::updateDrag(QPoint point) {
    if (m_lines.empty()) return;
    if (m_dragKind == DragKind::Line) SelectLinesTo(m_lineOffset + lineAt(point.y()), true);
    if (m_dragKind == DragKind::Text) MoveSelectionTo(positionAt(point), true);
}

void CaptionView::mouseMoveEvent(QMouseEvent* event) {
    const QPoint point = event->position().toPoint();
    viewport()->setCursor(point.x() < gutter ? Qt::ArrowCursor : Qt::IBeamCursor);
    if (m_dragKind != DragKind::None) {
        m_dragPoint = point;
        updateDrag(point);
        autoScroll();
    } else {
        const auto hover = point.x() < gutter && !m_lines.empty()
            ? std::optional<size_t>(m_lineOffset + lineAt(point.y())) : std::nullopt;
        if (m_hoverLine != hover) {
            m_hoverLine = hover;
            viewport()->update();
        }
    }
}

void CaptionView::autoScroll() {
    const int distance = m_dragPoint.y() - std::clamp(m_dragPoint.y(), 0, viewport()->height());
    if (!distance || m_dragKind == DragKind::None) {
        m_autoScroll.stop();
        return;
    }
    auto* bar = verticalScrollBar();
    bar->setValue(bar->value() + std::clamp(distance, -48, 48));
    updateDrag(m_dragPoint);
    if (!m_autoScroll.isActive()) m_autoScroll.start();
}

void CaptionView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragKind = DragKind::None;
        m_autoScroll.stop();
    } else {
        QAbstractScrollArea::mouseReleaseEvent(event);
    }
}

void CaptionView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || m_lines.empty()) return;
    m_dragKind = DragKind::None;
    m_autoScroll.stop();
    const auto point = event->position().toPoint();
    if (point.x() < gutter) {
        SelectLineThroughEnd(m_lineOffset + lineAt(point.y()));
    } else {
        bool inside = false;
        const auto position = positionAt(point, &inside);
        if (inside) SelectWordAt(position);
    }
}

bool CaptionView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::Leave && m_hoverLine) {
        m_hoverLine.reset();
        viewport()->update();
    } else if (event->type() == QEvent::UngrabMouse) {
        m_dragKind = DragKind::None;
        m_autoScroll.stop();
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void CaptionView::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::SelectAll)) SelectAll();
    else if (event->matches(QKeySequence::Copy)) emit copyRequested();
    else if (event->key() == Qt::Key_Escape) ClearSelection();
    else if (event->key() == Qt::Key_Home) verticalScrollBar()->setValue(0);
    else if (event->key() == Qt::Key_End) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    else return QAbstractScrollArea::keyPressEvent(event);
    event->accept();
}

void CaptionView::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    auto* copy = menu.addAction(HasSelection() ? tr("Copy selection\tCtrl+C") : tr("Copy all\tCtrl+C"),
                                this, &CaptionView::copyRequested);
    copy->setEnabled(!Empty());
    menu.addAction(tr("Select all\tCtrl+A"), this, &CaptionView::SelectAll)->setEnabled(!Empty());
    menu.addSeparator();
    menu.addAction(tr("Clear selection\tEsc"), this, &CaptionView::ClearSelection)->setEnabled(HasSelection());
    menu.exec(event->reason() == QContextMenuEvent::Keyboard
        ? viewport()->mapToGlobal(viewport()->rect().center()) : event->globalPos());
}
