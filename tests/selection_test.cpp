// Exercise transcript gestures in Qt logical coordinates without generating desktop input.
#include "ui/CaptionView.h"
#include <QApplication>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTest>
#include <QToolButton>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void Check(bool condition, const std::string& label) {
    std::printf("%-62s %s\n", label.c_str(), condition ? "PASS" : "FAIL");
    if (!condition) ++g_failures;
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t ch : text) {
        if (ch == L'\r') { out += "\\r"; continue; }
        if (ch == L'\n') { out += "\\n"; continue; }
        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return out;
}

void CheckText(const std::wstring& actual, const std::wstring& expected, const std::string& label) {
    Check(actual == expected, label);
    if (actual != expected) {
        std::printf("    expected [%s]\n    actual   [%s]\n", Narrow(expected).c_str(),
                    Narrow(actual).c_str());
    }
}

// The transcript joined the way a selection over all of it should come back.
std::wstring Joined(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) out += L"\r\n";
        out += lines[i];
    }
    return out;
}

void Mouse(QWidget* widget, QEvent::Type type, int x, int y, Qt::KeyboardModifiers mods = {}) {
    const QPoint local(x, y);
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, local, widget->mapToGlobal(local), button, held, mods);
    QCoreApplication::sendEvent(widget, &event);
}
void Down(QWidget* w, int x, int y, Qt::KeyboardModifiers mods = {}) { Mouse(w, QEvent::MouseButtonPress, x, y, mods); }
void Move(QWidget* w, int x, int y) { Mouse(w, QEvent::MouseMove, x, y); }
void Up(QWidget* w, int x, int y) { Mouse(w, QEvent::MouseButtonRelease, x, y); }
void DoubleClick(QWidget* w, int x, int y) {
    Down(w, x, y);
    Mouse(w, QEvent::MouseButtonDblClick, x, y);
    Up(w, x, y);
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget parent;
    CaptionView view(&parent);
    view.resize(760, 300);
    parent.resize(view.size());
    parent.show();
    view.SetTypography(L"Segoe UI", 14, 1.3);
    const std::vector<std::wstring> lines = {
        L"So we could learn where the system is folding in the real world.",
        L"Monitoring is missing, and the edge cases that exist in real scenarios "
        L"are the ones that wrap onto a second row in this pane.",
        L"All right.",
        L"So I guess that is my question.",
    };
    view.QueueUpdate(0, lines);
    view.Present();
    QTest::qWait(60);

    // --- Clear followed by an absolute-index update -----------------------
    // The capture merger retains its absolute transcript position when only
    // the pane is cleared. Revisions of that same line must replace one
    // another, not accumulate as progressive fragments.
    view.Clear();
    view.QueueUpdate(40, {L"Because"});
    view.QueueUpdate(40, {L"Because believing"});
    view.QueueUpdate(40, {L"Because believing that the dots will connect"});
    view.Present();
    CheckText(view.FullText(), L"Because believing that the dots will connect\r\n",
              "post-clear revisions replace the same absolute line");

    // Restore the standard fixture for the interaction checks below.
    view.Clear();
    view.QueueUpdate(0, lines);
    view.Present();

    // Gestures use Qt logical pixels at the same font size in every DPI configuration.
    constexpr int kGutterX = 10;
    constexpr int kFirstGlyphX = 121;
    constexpr int kFirstGlyphY = 14;
    constexpr int kPastEndY = 20000;
    // Rough Y of later rows at 14pt / 1.3 spacing. Line 1 wraps onto a second
    // row, so "All right" sits near y≈110 and the last line near y≈140.
    constexpr int kLine0Y = 20;
    constexpr int kLine2Y = 115;
    constexpr int kLine3Y = 145;

    // --- Dragging ----------------------------------------------------------
    Down(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Move(view.viewport(), 4000, kPastEndY);
    Up(view.viewport(), 4000, kPastEndY);
    CheckText(view.SelectedText(), Joined(lines), "drag past the last line selects everything");

    view.ClearSelection();
    Check(!view.HasSelection() && view.SelectedText().empty(), "clearing drops the selection");

    view.SelectAll();
    CheckText(view.SelectedText(), Joined(lines), "select all returns the whole transcript");

    // A drag that ends where it started is a plain click, and selects nothing.
    Down(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Up(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Check(!view.HasSelection(), "a plain click collapses the selection");

    // --- Words -------------------------------------------------------------
    DoubleClick(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    CheckText(view.SelectedText(), L"So", "double-click on a word selects that word");

    // Jitter between the double-click and the button coming up must not
    // collapse the word back to a caret.
    DoubleClick(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Move(view.viewport(), kFirstGlyphX + 1, kFirstGlyphY);
    Up(view.viewport(), kFirstGlyphX + 1, kFirstGlyphY);
    CheckText(view.SelectedText(), L"So", "a twitch after double-click keeps the word");

    DoubleClick(view.viewport(), 400, kPastEndY);
    Check(!view.HasSelection(), "double-click below the text selects no word");

    // --- Shift-click -------------------------------------------------------
    view.ClearSelection();
    Down(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Up(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Down(view.viewport(), 4000, kPastEndY, Qt::ShiftModifier);
    Up(view.viewport(), 4000, kPastEndY);
    CheckText(view.SelectedText(), Joined(lines), "shift-click extends from the last click");

    // --- Live updates: clamp ------------------------------------------------
    view.SelectAll();
    view.QueueUpdate(lines.size() - 1, {L"short"});
    view.Present();
    std::vector<std::wstring> revised = lines;
    revised.back() = L"short";
    CheckText(view.SelectedText(), Joined(revised),
              "selection survives the last line being revised");

    // --- Sticky end: grows with captions -----------------------------------
    view.ClearSelection();
    Down(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Move(view.viewport(), 4000, kPastEndY);
    Up(view.viewport(), 4000, kPastEndY);
    CheckText(view.SelectedText(), Joined(revised), "sticky setup reaches the transcript end");

    revised.back() = L"short and then longer again";
    view.QueueUpdate(revised.size() - 1, {revised.back()});
    view.Present();
    CheckText(view.SelectedText(), Joined(revised),
              "selection at end grows when the last line is rewritten");

    revised.push_back(L"a brand new line");
    view.QueueUpdate(revised.size() - 1, {revised.back()});
    view.Present();
    CheckText(view.SelectedText(), Joined(revised),
              "selection at end grows when a new line is appended");

    // A selection that stops short must not be pulled along.
    view.ClearSelection();
    DoubleClick(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    const std::wstring frozen = view.SelectedText();
    view.QueueUpdate(revised.size(), {L"should not join the selection"});
    view.Present();
    CheckText(view.SelectedText(), frozen, "selection short of the end stays put");

    // Shrinking the pane used to jump to the bottom whenever stick-to-bottom was
    // set (including when the transcript still fitted on screen), which scrolled
    // a mid-pane highlight out of sight and looked like the selection was gone.
    view.resize(view.width(), std::max(view.height() / 2, 80));
    QTest::qWait(40);
    CheckText(view.SelectedText(), frozen, "selection survives a caption pane height change");

    // --- Gutter line select ------------------------------------------------
    view.QueueUpdate(0, lines);
    view.Present();
    QTest::qWait(40);

    Down(view.viewport(), kGutterX, kLine2Y);
    Up(view.viewport(), kGutterX, kLine2Y);
    const std::wstring tailFromLine2 = lines[2] + L"\r\n" + lines[3];
    CheckText(view.SelectedText(), tailFromLine2,
              "gutter click selects from the clicked line through the transcript end");

    const std::wstring appendedCaption = L"A new caption after the gutter click.";
    view.QueueUpdate(lines.size(), {appendedCaption});
    view.Present();
    CheckText(view.SelectedText(), tailFromLine2 + L"\r\n" + appendedCaption,
              "gutter tail selection grows with new caption text");

    view.QueueUpdate(0, lines);
    view.Present();

    Down(view.viewport(), kGutterX, kLine0Y);
    Move(view.viewport(), kGutterX, kLine2Y);
    Up(view.viewport(), kGutterX, kLine2Y);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "gutter drag down takes whole lines");

    Down(view.viewport(), kGutterX, kLine2Y);
    Move(view.viewport(), kGutterX, kLine0Y);
    Up(view.viewport(), kGutterX, kLine0Y);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "gutter drag up flips the anchor so the origin row stays whole");

    view.ClearSelection();
    Down(view.viewport(), kGutterX, kLine0Y);
    Up(view.viewport(), kGutterX, kLine0Y);
    Down(view.viewport(), kGutterX, kLine3Y, Qt::ShiftModifier);
    Up(view.viewport(), kGutterX, kLine3Y);
    CheckText(view.SelectedText(), Joined(lines), "shift-click in the gutter extends by lines");

    // Gutter drag stays line-granular after the pointer wanders over the text.
    Down(view.viewport(), kGutterX, kLine0Y);
    Move(view.viewport(), kFirstGlyphX + 80, kLine2Y);
    Up(view.viewport(), kFirstGlyphX + 80, kLine2Y);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "gutter drag stays line-granular over the text");

    // Text drag never turns into a line selection just by crossing the gutter.
    Down(view.viewport(), kFirstGlyphX, kFirstGlyphY);
    Move(view.viewport(), kGutterX, kLine2Y);
    Up(view.viewport(), kGutterX, kLine2Y);
    {
        const std::wstring selected = view.SelectedText();
        Check(view.HasSelection() && selected.find(L"So we could learn") == 0 &&
                  selected != lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "text drag through the gutter stays character-granular");
    }

    DoubleClick(view.viewport(), kGutterX, kLine0Y);
    Check(view.SelectedText() == Joined(lines),
          "double-click in the gutter selects the transcript tail rather than sending");

    // --- Trimming ----------------------------------------------------------
    // The pane keeps a bounded number of lines; a selection anchored at the top
    // has to follow the window rather than point at a line that is gone.
    {
        std::vector<std::wstring> many;
        for (int i = 0; i < 3000; ++i) {
            wchar_t text[32];
            ::swprintf_s(text, L"line %04d", i);
            many.emplace_back(text);
        }
        view.QueueUpdate(0, many);
        view.SelectAll();
        view.QueueUpdate(many.size(), {L"one more line"});
        view.Present();

        const std::wstring selected = view.SelectedText();
        const size_t firstBreak = selected.find(L"\r\n");
        const std::wstring firstLine =
            firstBreak == std::wstring::npos ? selected : selected.substr(0, firstBreak);
        Check(view.HasSelection(), "selection survives lines being trimmed away");
        CheckText(firstLine, L"line 0001", "a trimmed-away anchor moves to the oldest kept line");
        Check(selected.find(L"one more line") != std::wstring::npos,
              "select-all at the end grows across the trim append");
        QTest::keyClick(&view, Qt::Key_Home);
        auto* jump = view.findChild<QToolButton*>("jumpToLatest");
        Check(view.verticalScrollBar()->value() == 0 && jump && jump->isVisible(),
              "Home reveals the jump-to-latest control");
        if (jump) jump->click();
        Check(view.verticalScrollBar()->value() == view.verticalScrollBar()->maximum(),
              "jump to latest restores the bottom position");
        QTest::keyClick(&view, Qt::Key_PageUp);
        Check(view.verticalScrollBar()->value() < view.verticalScrollBar()->maximum(), "Page Up scrolls the transcript");
        QTest::keyClick(&view, Qt::Key_End);
        Check(view.verticalScrollBar()->value() == view.verticalScrollBar()->maximum(), "End scrolls to the last caption");
    }

    // --- Keyboard ----------------------------------------------------------
    view.QueueUpdate(0, lines);
    view.Present();

    int copies = 0;
    QObject::connect(&view, &CaptionView::copyRequested, [&] { ++copies; });
    view.ClearSelection();
    QTest::keyClick(&view, Qt::Key_A, Qt::ControlModifier);
    CheckText(view.SelectedText(), Joined(lines), "Ctrl+A selects the whole transcript");
    QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
    Check(copies == 1, "Ctrl+C asks the parent to copy");
    QTest::keyClick(&view, Qt::Key_C);
    Check(copies == 1 && view.HasSelection(), "a bare C neither copies nor deselects");
    QTest::keyClick(&view, Qt::Key_Escape);
    Check(!view.HasSelection(), "Escape clears the selection");

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "All checks passed" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
