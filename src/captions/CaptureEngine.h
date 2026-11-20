#pragma once

#include <windows.h>
#include <QEvent>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "CaptionMerger.h"
#include "CaptionSource.h"
#include "TranscriptStore.h"

// Posted from the capture thread. Qt owns the payload until the UI consumes it.
struct CaptionUpdate final : QEvent {
    static QEvent::Type eventType();
    CaptionUpdate(size_t first, std::vector<std::wstring> replacement);
    size_t firstDirtyLine;
    std::vector<std::wstring> lines;
};

struct CaptionStatus final : QEvent {
    static QEvent::Type eventType();
    explicit CaptionStatus(const std::wstring& message);
    std::wstring text;
};

// Owns the background polling thread that scrapes the caption window, merges
// snapshots into a transcript, and streams results to the UI.
//
// Unlike the Python version, every wait is performed on a stop event, so Stop()
// returns promptly even while the engine is searching for a caption window.
class CaptureEngine {
public:
    ~CaptureEngine();

    CaptureEngine() = default;
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // Stop joins the producer before its receiver can be destroyed.
    bool Start(QObject* receiver, const std::wstring& transcriptPath, int pollIntervalMs,
               CaptionSourceChoice sourceChoice);
    void Stop();
    void SetPaused(bool paused);

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    bool IsPaused() const { return m_paused.load(std::memory_order_acquire); }
    HWND SourceWindow() const { return m_sourceWindow.load(std::memory_order_acquire); }

private:
    void Run(std::wstring transcriptPath);
    bool Sleep(DWORD milliseconds);  // returns false when asked to stop
    // Blocks until the caption text changes, the safety poll expires, or the
    // engine is stopped. Returns false only when asked to stop.
    bool AwaitChange();
    void PostStatus(const std::wstring& text);

    // Measured: Windows 11 Live captions accepts a change subscription but never
    // raises an event, so polling is not a fallback there, it is the mechanism.
    // The interval is therefore configurable and tight by default.
    static constexpr DWORD kDefaultPollIntervalMs = 8;
    static constexpr DWORD kSearchIntervalMs = 100;
    // Floor between consecutive reads, so a provider that does raise events
    // rapidly cannot spin the thread.
    static constexpr DWORD kMinReadIntervalMs = 2;

    DWORD              m_pollIntervalMs = kDefaultPollIntervalMs;
    CaptionSourceChoice m_sourceChoice = CaptionSourceChoice::WindowsLiveCaptions;
    bool               m_sawChangeEvent = false;
    std::thread        m_thread;
    HANDLE             m_stopEvent = nullptr;
    HANDLE             m_changeEvent = nullptr;
    HANDLE             m_controlEvent = nullptr;
    QObject*           m_receiver = nullptr;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_paused{false};
    std::atomic<HWND>  m_sourceWindow{nullptr};

    CaptionSource   m_source;
    CaptionMerger   m_merger;
    TranscriptStore m_store;
};
