// Deterministic tests of the real worker/preview queue. Fake browser responses
// never inspect the desktop, move the pointer, change focus, or type anything.
#define WEBINPUT_PICKER_TESTING
#include "windows/WebInputPicker.cpp"
#include "ui/CaptionTargetList.h"
#include "resource.h"

#include <atomic>
#include <chrono>
#include <commctrl.h>
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
    const auto state = picker.Inspect({-32000, -32000}, nullptr, true);
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
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    ::InitCommonControlsEx(&controls);
    HWND parent = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 320, 220,
                                    nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create a private target-list window");
    if (!parent) return;
    WebInputPicker picker;
    HWND first = nullptr;
    {
        int changes = 0;
        std::wstring status;
        CaptionTargetList list(picker);
        Check(list.Create(parent, [&] { ++changes; }, [&](const std::wstring& text) { status = text; }),
              "create the original hidden target tile");
        first = ::GetDlgItem(parent, IDC_SELECTED_WINDOW);
        Access::AddTestTabs(picker, parent);
        list.SetBounds({0, 25, 240, 200}, 96);
        list.Refresh();
        HWND second = ::FindWindowExW(parent, nullptr, L"STATIC", L"Selected window: Second tab");
        RECT firstRect{}, secondRect{};
        Check(first && second && ::GetWindowRect(first, &firstRect) &&
              ::GetWindowRect(second, &secondRect) && secondRect.top > firstRect.bottom,
              "picked tabs have distinct controls in the saved order");

        const auto click = [](HWND tile, int x, int y) {
            ::SendMessageW(tile, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            ::SendMessageW(tile, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        };
        click(second, 8, 8);
        Check(picker.SelectedName() == L"Second tab" && changes == 1, "selecting a tile updates the send target");
        ::SendMessageW(second, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(8, 8));
        Check(list.CancelRemoval() && picker.PickedCount() == 2, "cancelling a drag keeps both tabs");
        const int closeX = secondRect.right - secondRect.left - 10;
        const int closeY = (secondRect.bottom - secondRect.top) / 2;
        click(second, closeX, closeY);
        Check(picker.SelectedName() == L"First tab" && status == L"Selected tab removed." &&
              changes == 2, "closing a tab selects the surviving target");
        click(first, closeX, closeY);
        Check(picker.PickedCount() == 0 && ::GetDlgItem(parent, IDC_SELECTED_WINDOW) == first &&
              !(::GetWindowLongPtrW(first, GWL_STYLE) & WS_VISIBLE), "empty list keeps its original hidden tile");
    }
    Check(!::IsWindow(first), "destroying the list releases its child controls");
    ::DestroyWindow(parent);
}
}  // namespace

int main() {
    BusyWorkerAndCache();
    ActiveScanAndWindowSwitch();
    Cancellation();
    FreshDropPriority();
    RetryAndOwnership();
    TargetListSelectionAndRemoval();
    std::printf("\nPicker async: %d failures\n", failures);
    return failures ? 1 : 0;
}
