// Deterministic tests of the real worker/preview queue. Fake browser responses
// never inspect the desktop, move the pointer, change focus, or type anything.
#define WEBINPUT_PICKER_TESTING
#include "windows/WebInputPicker.cpp"
#include "ui/CaptionTargetList.h"
#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QTest>
#include <QToolButton>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>

struct WebInputPickerTestAccess {
    using Impl = WebInputPicker::Impl;
    using Request = Impl::HoverRequest;
    using Snapshot = Impl::Snapshot;
    static Impl& Get(WebInputPicker& picker) { return *picker.m_impl; }
    static WebInputPickState Preview(WebInputPicker& picker, HWND window,
                                    ULONGLONG now = ::GetTickCount64()) {
        return Get(picker).Preview({10, 10}, nullptr, window,
                                  WebInputPickState::Checking, now);
    }
    static Snapshot Valid(const Request& request) {
        return {WebInputPickState::Valid, request.window, L"test composer", true, nullptr, {}};
    }
    static bool Complete(WebInputPicker& picker) {
        auto& impl = Get(picker);
        std::lock_guard lock(impl.mutex);
        return impl.completedHover.has_value();
    }
    static HWND PendingWindow(WebInputPicker& picker) {
        auto& impl = Get(picker);
        std::lock_guard lock(impl.mutex);
        return impl.pendingHover ? impl.pendingHover->window : nullptr;
    }
    static void Drain(WebInputPicker& picker) {
        Get(picker).Invoke([](PickerAutomation&) {});
    }
    static void AddTestTabs(WebInputPicker& picker, HWND source) {
        Get(picker).Update([source](PickerAutomation& automation) {
            for (const auto* name : {L"First tab", L"Second tab"}) {
                PickerAutomation::Target tab;
                tab.hwnd = source;
                tab.name = name;
                automation.picked.push_back(std::move(tab));
            }
            automation.SelectPicked(0);
        });
    }
};

namespace {
using Access = WebInputPickerTestAccess;
using Clock = std::chrono::steady_clock;
int failures = 0;
const HWND windowA = reinterpret_cast<HWND>(1);
const HWND windowB = reinterpret_cast<HWND>(2);
const HWND windowC = reinterpret_cast<HWND>(3);

void Check(bool passed, const char* name) {
    std::printf("%-76s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) ++failures;
}

template <typename Condition>
bool Until(Condition condition) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!condition()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

double Elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Gate {
    std::promise<void> entered;
    std::future<void> entry = entered.get_future();
    std::promise<void> released;
    std::shared_future<void> release = released.get_future().share();
    std::atomic<bool> timedOut{false};
    void Block() {
        entered.set_value();
        if (release.wait_for(std::chrono::seconds(3)) != std::future_status::ready) timedOut = true;
    }
    bool WaitForEntry() {
        return entry.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    }
    void Open() { released.set_value(); }
};

void BusyWorkerAndCache() {
    Gate gate;
    WebInputPicker picker;
    std::atomic<int> calls{0};
    Access::Get(picker).inspectHoverForTesting = [&](const Access::Request& request) {
        ++calls;
        return Access::Valid(request);
    };
    Access::Get(picker).Post([&](PickerAutomation&) { gate.Block(); });
    Check(gate.WaitForEntry(), "worker is deliberately held busy");
    auto start = Clock::now();
    const auto first = Access::Preview(picker, windowA);
    const double firstMs = Elapsed(start);
    Check(first == WebInputPickState::Checking && firstMs < 50,
          "preview returns without waiting for the busy worker");
    start = Clock::now();
    for (int i = 0; i < 1000; ++i) Access::Preview(picker, windowA);
    const double burstMs = Elapsed(start);
    Check(burstMs < 100, "1000 mouse previews do not wait for a browser response");
    Access::Preview(picker, windowB);
    Access::Preview(picker, windowC);
    Check(Access::PendingWindow(picker) == windowC && calls == 0,
          "queued mouse previews coalesce to the latest window");
    gate.Open();
    Check(Until([&] { return Access::Complete(picker); }), "latest queued preview completes");
    Check(Access::Preview(picker, windowC) == WebInputPickState::Valid &&
              picker.CandidateWindow() == windowC && calls == 1,
          "only the latest window is inspected and displayed");
    start = Clock::now();
    for (int i = 0; i < 1000; ++i) Access::Preview(picker, windowC);
    const double cachedMs = Elapsed(start);
    Check(calls == 1 && !Access::PendingWindow(picker),
          "cached mouse previews make no additional browser calls");
    Check(!picker.CommitCandidate() && !picker.SelectedWindow(),
          "a valid hover preview alone cannot commit a target");
    const auto expired = Access::Get(picker).hoverCompletedAt + webinput::kInspectionCacheMs;
    Access::Preview(picker, windowC, expired);
    Check(Until([&] { return Access::Complete(picker); }) && calls == 2,
          "successful previews refresh when their completion-based cache expires");
    Check(!gate.timedOut, "response gate was released by the test, not its timeout");
    std::printf("Timing: busy preview %.3f ms; 1000 queued %.3f ms; 1000 cached %.3f ms\n",
                firstMs, burstMs, cachedMs);
}

void ActiveScanAndWindowSwitch() {
    Gate gate;
    WebInputPicker picker;
    std::atomic<int> callsA{0}, callsB{0}, callsC{0};
    Access::Get(picker).inspectHoverForTesting = [&](const Access::Request& request) {
        if (request.window == windowA) { ++callsA; gate.Block(); }
        if (request.window == windowB) ++callsB;
        if (request.window == windowC) ++callsC;
        return Access::Valid(request);
    };
    Access::Preview(picker, windowA);
    Check(gate.WaitForEntry(), "simulated browser scan is in flight");
    Access::Preview(picker, windowB);
    Check(Access::Preview(picker, windowC) == WebInputPickState::Checking &&
              !picker.CandidateWindow(),
          "switching windows immediately hides the previous candidate");
    gate.Open();
    Check(Until([&] { return Access::Complete(picker); }), "new window scan completes after old scan");
    Check(Access::Preview(picker, windowC) == WebInputPickState::Valid &&
              picker.CandidateWindow() == windowC && callsA == 1 && callsB == 0 && callsC == 1,
          "stale in-flight result is discarded and intermediate window is skipped");
    Check(!gate.timedOut, "window switch does not rely on a scan timeout");
}

void Cancellation() {
    Gate gate;
    WebInputPicker picker;
    Access::Get(picker).snapshot.selectedWindow = windowC;
    Access::Get(picker).snapshot.selectedName = L"retained target";
    Access::Get(picker).inspectHoverForTesting = [&](const Access::Request& request) {
        gate.Block();
        return Access::Valid(request);
    };
    Access::Preview(picker, windowA);
    Check(gate.WaitForEntry(), "cancel test starts with an active scan");
    const auto start = Clock::now();
    picker.ResetCandidate();
    Check(Elapsed(start) < 50 && !picker.CandidateWindow() && !picker.CandidateValid(),
          "cancel clears candidate immediately without waiting for accessibility");
    Check(picker.SelectedWindow() == windowC && picker.SelectedName() == L"retained target",
          "cancelling a preview preserves the previously selected target");
    gate.Open();
    Access::Drain(picker);
    Check(!Access::Complete(picker) && !picker.CandidateWindow(),
          "late completion cannot resurrect a cancelled candidate");
    Check(!gate.timedOut, "cancel does not rely on a scan timeout");
}

void FreshDropPriority() {
    Gate gate;
    WebInputPicker picker;
    std::atomic<int> calls{0};
    Access::Get(picker).inspectHoverForTesting = [&](const Access::Request& request) {
        ++calls;
        gate.Block();
        return Access::Valid(request);
    };
    Access::Preview(picker, windowA);
    Check(gate.WaitForEntry(), "drop test starts with an active scan");
    Access::Preview(picker, windowB);
    // Let the in-flight provider return once Inspect has discarded the queued
    // hover. The drop still uses real native hit testing at an off-desktop point.
    std::thread release([&] {
        Until([&] { return !Access::PendingWindow(picker); });
        gate.Open();
    });
    // Off-screen helpers can occupy negative coordinates. No window's
    // exclusive right or bottom edge can extend past LONG_MAX.
    const auto state = picker.Inspect({LONG_MAX, LONG_MAX}, nullptr, true);
    release.join();
    Check(state == WebInputPickState::NoWindow && calls == 1,
          "fresh drop supersedes queued previews and checks the actual release point");
    Check(!picker.CommitCandidate() && !picker.SelectedWindow(),
          "invalid final drop cannot commit the previously valid hover");
    Check(!Access::Complete(picker) && !gate.timedOut,
          "an older preview cannot overwrite final drop validation");
}

void RetryAndOwnership() {
    WebInputPicker picker;
    std::atomic<int> calls{0};
    Access::Get(picker).inspectHoverForTesting = [&](const Access::Request& request) {
        if (++calls == 1) {
            auto result = Access::Valid(request);
            result.state = WebInputPickState::NoEditableInput;
            result.candidateWindow = nullptr;
            result.candidateValid = false;
            return result;
        }
        return Access::Valid(request);
    };
    Access::Preview(picker, windowA);
    Check(Until([&] { return Access::Complete(picker); }), "negative browser result completes");
    Check(Access::Preview(picker, windowA) == WebInputPickState::NoEditableInput,
          "negative result is reported without allowing a drop");
    for (int i = 0; i < 100; ++i) Access::Preview(picker, windowA);
    Check(calls == 1, "negative results are briefly cached rather than repeatedly queried");
    Access::Preview(picker, windowA,
                    Access::Get(picker).hoverCompletedAt + webinput::kInspectionCacheMs);
    Check(Until([&] { return Access::Complete(picker); }), "negative preview retries after expiry");
    Check(Access::Preview(picker, windowA) == WebInputPickState::Valid && calls == 2,
          "a later appearing editor becomes selectable without restarting the drag");

    picker.ResetCandidate();
    Access::Drain(picker);
    Access::Get(picker).inspectHoverForTesting = [](const Access::Request& request) {
        auto result = Access::Valid(request);
        result.candidateWindow = windowB;
        return result;
    };
    Access::Preview(picker, windowA);
    Check(Until([&] {
        auto& impl = Access::Get(picker);
        std::lock_guard lock(impl.mutex);
        return !impl.pendingHover && !impl.runningHover;
    }), "mismatched browser response finishes");
    Check(!Access::Complete(picker) && !picker.CandidateWindow(),
          "result for a different native window is never displayed");
}
void TargetListSelectionAndRemoval() {
    QWidget parent;
    parent.resize(320, 220);
    WebInputPicker picker;
    QPointer<QFrame> first;
    {
        int changes = 0;
        QString status;
        CaptionTargetList list(picker, &parent);
        list.setGeometry(0, 25, 240, 175);
        QObject::connect(&list, &CaptionTargetList::selectionChanged, [&] { ++changes; });
        QObject::connect(&list, &CaptionTargetList::statusChanged, [&](const QString& value) { status = value; });
        first = list.findChild<QFrame*>("captionTargetTile");
        Check(first && first->isHidden(), "empty target list has a hidden tile");
        const auto hasVisibleText = [&list] {
            for (auto* label : list.findChildren<QLabel*>()) {
                if (label->isVisibleTo(&list) && !label->text().isEmpty()) return true;
            }
            return false;
        };
        Check(!hasVisibleText(), "empty target list starts blank");
        Access::AddTestTabs(picker, reinterpret_cast<HWND>(parent.winId()));
        list.Refresh();
        parent.show();
        QCoreApplication::processEvents();
        auto tiles = list.findChildren<QFrame*>("captionTargetTile");
        if (!first || tiles.size() != 2) { Check(false, "create two picked tab controls"); return; }
        auto* second = tiles.at(1);
        auto* name = second->findChild<QLabel*>("targetName");
        auto* icon = second->findChild<QLabel*>("targetIcon");
        Check(name && name->isVisible() && name->text() == "Second tab", "picked target name is visible");
        list.resize(72, list.height());
        QCoreApplication::processEvents();
        Check(name && icon && name->geometry().top() > icon->geometry().bottom()
              && name->width() >= name->fontMetrics().horizontalAdvance("Second"),
              "narrow sidebar gives the target name its own row");
        list.resize(240, list.height());
        QCoreApplication::processEvents();
        Check(second->accessibleName() == "Selected window: Second tab" &&
              second->geometry().top() > first->geometry().bottom(),
              "picked tabs have distinct controls in the saved order");
        QTest::mouseClick(second, Qt::LeftButton, {}, QPoint(8, 8));
        Check(picker.SelectedName() == L"Second tab" && changes == 1, "selecting a tile updates the send target");
        const auto dragSecondOverFirst = [&] {
            QTest::mousePress(second, Qt::LeftButton, {}, QPoint(8, 8));
            const QPoint drop = first->mapToGlobal(first->rect().center());
            QMouseEvent move(QEvent::MouseMove, second->mapFromGlobal(drop), drop,
                             Qt::NoButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(second, &move);
            QTest::mouseRelease(second, Qt::LeftButton, {}, QPoint(8, 8));
            QCoreApplication::processEvents();
        };
        dragSecondOverFirst();
        Check(picker.PickedName(0) == L"Second tab" && picker.SelectedName() == L"Second tab" &&
              second->geometry().top() < first->geometry().top(), "reordering moves the tile and preserves the active target");
        dragSecondOverFirst();
        Check(picker.PickedName(0) == L"First tab", "dragging the tile back restores the original order");
        QTest::mousePress(second, Qt::RightButton, {}, QPoint(8, 8));
        Check(list.CancelRemoval() && picker.PickedCount() == 2, "cancelling a drag keeps both tabs");
        QTest::mouseRelease(second, Qt::RightButton, {}, QPoint(8, 8));
        second->findChild<QToolButton*>("closeTarget")->click();
        Check(picker.SelectedName() == L"First tab" && status == "Selected tab removed." &&
              changes == 2, "closing a tab selects the surviving target");
        QTest::mousePress(first, Qt::RightButton, {}, QPoint(8, 8));
        const QPoint outside = parent.frameGeometry().bottomRight() + QPoint(50, 50);
        QMouseEvent release(QEvent::MouseButtonRelease, first->mapFromGlobal(outside), outside,
                            Qt::RightButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(first, &release);
        Check(picker.PickedCount() == 0 && first && first->isHidden(),
              "right-dragging outside removes the tab and leaves an empty list");
        Check(!hasVisibleText(), "removing the last target leaves the list blank");
    }
    Check(first.isNull(), "destroying the list releases its child controls");
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    BusyWorkerAndCache();
    ActiveScanAndWindowSwitch();
    Cancellation();
    FreshDropPriority();
    RetryAndOwnership();
    TargetListSelectionAndRemoval();
    std::printf("\nPicker async: %d failures\n", failures);
    return failures ? 1 : 0;
}
