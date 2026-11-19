#pragma once

#include <windows.h>

#include <QAbstractNativeEventFilter>
#include <QWidget>

#include <string>

#include "windows/ActiveWindowFollower.h"
#include "CaptionView.h"
#include "CaptionTargetList.h"
#include "captions/CaptureEngine.h"
#include "captions/Settings.h"
#include "windows/WebInputPicker.h"

class CaptionPane final : public QWidget, private QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit CaptionPane(QWidget* parent);
    ~CaptionPane() override;
    bool initialize(const Settings& settings);
    void setLogVisible(bool visible);

signals:
    void logLine(const QString& text);
    void logToggled(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

private:
    static bool RegisterPickOutlineClass(HINSTANCE instance);
    static LRESULT CALLBACK ControlMessageProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK PickButtonSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                                   DWORD_PTR);
    static LRESULT CALLBACK PickOutlineWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleControlMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateChildren();
    void PopulateFontCombo();
    void PopulateSizeCombo();
    void PopulateSpacingCombo();
    void ApplyControlFont();
    void ApplyTheme();
    void EnsureThemeBrushes();
    void DiscardThemeBrushes();
    void DrawDarkButton(const DRAWITEMSTRUCT& item) const;
    LRESULT OnCtlColor(HDC dc, HWND control);
    int  LogBarHeight() const;
    int  ClampBottomPanelHeight(int wanted, int clientHeight) const;
    int  ClampSidePanelWidth(int wanted, int clientWidth) const;
    void DragSplitterTo(int splitterTop);
    void DragSideSplitterTo(int splitterLeft);
    void Layout();
    void UpdateHotkeyRegistration();
    void QueueHotkeySend();
    bool HotkeyChordReleased() const;
    void UpdateWindowTitle();
    void SetStatus(const std::wstring& text);
    void ApplyTypography();

    void OnCommand(int controlId, int notifyCode);
    void OnSend();
    void OnSave();
    void OnClear();
    void OnToggleListening();
    void UpdateListeningButton();
    void OnSettings();
    void OnPickWindow();
    void BeginWindowPick();
    void UpdateWindowPick(POINT screenPoint, bool forceRefresh = false);
    void FinishWindowPick(bool accept);
    void ShowWindowPickStatus(WebInputPickState state);
    void UpdatePickOutline(HWND window);
    void HidePickOutline();
    void OnCopy();
    void OnCaptionUpdate(CaptionUpdate* update);
    void MaybeCopyRealtime();
    void DrainPendingPayloads();

    bool CopyTextToClipboard(const std::wstring& text);
    int  Scaled(int value) const;
    int  Unscaled(int value) const;

    HWND      m_hwnd = nullptr;
    HWND      m_frame = nullptr;
    bool      m_initialized = false;
    HINSTANCE m_instance = nullptr;
    UINT      m_dpi = 96;
    HFONT     m_controlFont = nullptr;
    bool      m_hotkeyRegistered = false;
    bool      m_hotkeySendPending = false;
    bool      m_darkTheme = true;

    HWND m_sendButton = nullptr;
    HWND m_pressEnterCheck = nullptr;
    HWND m_fontCombo = nullptr;
    HWND m_sizeCombo = nullptr;
    HWND m_spacingCombo = nullptr;
    HWND m_rightPanel = nullptr;
    HWND m_bottomPanel = nullptr;
    HWND m_saveButton = nullptr;
    HWND m_clearButton = nullptr;
    HWND m_listenButton = nullptr;
    HWND m_settingsButton = nullptr;
    HWND m_logButton = nullptr;
    HWND m_pickWindowButton = nullptr;
    HWND m_pickOutlineWindow = nullptr;
    bool m_logVisible = false;

    WebInputPicker m_webInputPicker;
    CaptionTargetList m_targets{m_webInputPicker};
    bool m_windowPickDrag = false;
    bool m_windowPickUpdating = false;
    WebInputPickState m_windowPickState = WebInputPickState::NoWindow;

    HBRUSH m_windowBrush = nullptr;
    HBRUSH m_panelBrush = nullptr;
    HBRUSH m_buttonBrush = nullptr;
    HBRUSH m_buttonPressedBrush = nullptr;

    // Grab strip between the caption pane and the bottom panel. Parent area, so
    // the drag is handled here rather than by a child control.
    RECT m_splitterRect{};
    bool m_splitterDrag = false;
    int  m_splitterGrab = 0;  // cursor offset inside the strip when the drag began

    // Grab strip on the right edge of the left panel.
    RECT m_sideSplitterRect{};
    bool m_sideSplitterDrag = false;
    int  m_sideSplitterGrab = 0;

    CaptionView   m_view;
    CaptureEngine m_engine;
    Settings      m_settings;
    ActiveWindowFollower m_activeWindowFollower;

    ULONGLONG m_lastRealtimeCopyTick = 0;
};
