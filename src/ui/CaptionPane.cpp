#include "CaptionPane.h"

#include <QApplication>
#include <QFocusEvent>
#include <QPainter>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <utility>

#include "captions/CaptionSave.h"
#include "CaptionTheme.h"
#include "SettingsDialog.h"
#include "captions/Util.h"
#include "resource.h"

namespace {

constexpr const wchar_t* kPickOutlineClassName = L"LiveCaptionViewPickOutline";
constexpr const wchar_t* kBaseTitle = L"Live Caption App";

// Unscaled (96 dpi) toolbar geometry. The bottom panel's minimum height is
// exactly the button row plus the top/bottom insets used when laying it out.
constexpr int kPad          = 8;
constexpr int kRowHeight    = 23;
constexpr int kBarTopPad    = 4;
constexpr int kBarBottomPad = 5;
constexpr int kBarHeight    = kBarTopPad + kRowHeight + kBarBottomPad;
constexpr int kGap          = 6;
constexpr int kSendWidth    = 92;
// Left panel: the pick button and the selected tab (icon plus title). The
// stored width starts here; dragging the panel's right edge changes it.
constexpr int kMinSidePanelWidth = 56;
constexpr int kSideButtonSize = 32;
constexpr int kSideSplitterWidth = 6;
// Narrowest caption column a side-panel drag may leave behind.
constexpr int kMinColumnWidth = 320;
// Extends the native title area into the client region. Besides creating
// breathing room above the captions, this strip behaves like draggable title
// bar space in normal view. Compact view intentionally omits it.
constexpr int kTitleBarExtension = 28;
// Draggable strip between the caption pane and the bottom panel, and the
// smallest caption pane a drag may leave behind.
constexpr int kSplitterHeight  = 6;
constexpr int kMinViewHeight   = 80;

// Throttle for "copy real-time"; the caption updates ~10x per second and we do
// not want to thrash the clipboard that hard.
constexpr ULONGLONG kRealtimeCopyIntervalMs = 600;
constexpr UINT_PTR kHotkeySendTimerId = 0xCA51;
constexpr UINT kHotkeySendPollMs = 10;
constexpr UINT_PTR kWindowPickTimerId = 0xCA52;
constexpr UINT kWindowPickPollMs = 50;
constexpr UINT_PTR kActiveWindowFollowTimerId = 0xCA53;
constexpr UINT kActiveWindowFollowPollMs = 10;

int CALLBACK EnumFontProc(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD, LPARAM param) {
    auto* names = reinterpret_cast<std::set<std::wstring>*>(param);
    if (logFont->lfFaceName[0] != L'@') names->insert(logFont->lfFaceName);
    return 1;
}

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD bytes = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &bytes) ==
               ERROR_SUCCESS &&
           value == 0;
}

bool EffectiveDarkTheme(UiTheme theme) {
    return theme == UiTheme::Dark || (theme == UiTheme::System && SystemUsesDarkTheme());
}

std::wstring HotkeyName(unsigned modifiers, unsigned key) {
    std::wstring name;
    auto append = [&name](const wchar_t* part) {
        if (!name.empty()) name += L"+";
        name += part;
    };

    if (modifiers & MOD_CONTROL) append(L"Ctrl");
    if (modifiers & MOD_ALT) append(L"Alt");
    if (modifiers & MOD_SHIFT) append(L"Shift");
    if (modifiers & MOD_WIN) append(L"Win");

    if (key >= 'A' && key <= 'Z') {
        const wchar_t keyName[] = {static_cast<wchar_t>(key), L'\0'};
        append(keyName);
    } else if (key >= '0' && key <= '9') {
        const wchar_t keyName[] = {static_cast<wchar_t>(key), L'\0'};
        append(keyName);
    } else if (key != 0) {
        wchar_t keyName[64]{};
        UINT scanCode = ::MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        switch (key) {
            case VK_INSERT:
            case VK_DELETE:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_DIVIDE:
            case VK_NUMLOCK:
                scanCode |= 0x100;
                break;
        }
        if (::GetKeyNameTextW(static_cast<LONG>(scanCode << 16), keyName,
                              static_cast<int>(std::size(keyName))) > 0) {
            append(keyName);
        } else {
            append(L"Unknown");
        }
    }

    return name.empty() ? L"None" : name;
}

CaptionSourceChoice ToSourceChoice(CaptionSourcePreference source) {
    return source == CaptionSourcePreference::ChromeLiveCaption
               ? CaptionSourceChoice::Chrome
               : CaptionSourceChoice::WindowsLiveCaptions;
}

HWND captionTabStop(HWND pane, bool last) {
    HWND first = ::GetNextDlgTabItem(pane, nullptr, FALSE);
    return first && last ? ::GetNextDlgTabItem(pane, first, TRUE) : first;
}

QColor widgetColor(COLORREF color) {
    return {GetRValue(color), GetGValue(color), GetBValue(color)};
}

}  // namespace

bool CaptionPane::RegisterPickOutlineClass(HINSTANCE instance) {
    WNDCLASSEXW outline{};
    outline.cbSize = sizeof(outline);
    outline.lpfnWndProc = &CaptionPane::PickOutlineWndProc;
    outline.hInstance = instance;
    outline.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
    outline.hbrBackground = nullptr;
    outline.lpszClassName = kPickOutlineClassName;
    return ::RegisterClassExW(&outline) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int CaptionPane::Scaled(int value) const {
    return ::MulDiv(value, static_cast<int>(m_dpi), 96);
}

int CaptionPane::Unscaled(int value) const {
    return ::MulDiv(value, 96, static_cast<int>(m_dpi));
}

CaptionPane::CaptionPane(QWidget* parent) : QWidget(parent) {
    setObjectName("captionPane");
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(660, 175);
}

CaptionPane::~CaptionPane() {
    qApp->removeNativeEventFilter(this);
    // Native controls call back into the view and target list during teardown.
    // Release the widget's window while those members are still alive.
    if (m_hwnd) destroy();
}

bool CaptionPane::initialize(const Settings& settings) {
    if (m_hwnd) return m_initialized;
    m_instance = ::GetModuleHandleW(nullptr);
    if (!RegisterPickOutlineClass(m_instance)) return false;
    const HWND pane = reinterpret_cast<HWND>(winId());
    if (!pane || !::SetWindowSubclass(pane, &ControlMessageProc, 1,
                                      reinterpret_cast<DWORD_PTR>(this))) return false;
    m_hwnd = pane;
    m_frame = ::GetAncestor(pane, GA_ROOT);
    m_settings = settings;
    m_dpi = ::GetDpiForWindow(pane);
    if (!m_dpi) m_dpi = 96;
    EnsureThemeBrushes();
    if (!CreateChildren()) return false;
    ApplyTheme();
    UpdateHotkeyRegistration();
    Layout();
    ApplyTypography();
    if (!::SetTimer(pane, kActiveWindowFollowTimerId, kActiveWindowFollowPollMs, nullptr))
        return false;
    m_engine.Start(pane, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs,
                   ToSourceChoice(m_settings.captionSource));
    SetStatus(std::wstring(L"Saving transcript to ") + m_settings.ResolvedTranscriptPath());
    qApp->installNativeEventFilter(this);
    m_initialized = true;
    return true;
}

LRESULT CALLBACK CaptionPane::ControlMessageProc(HWND hwnd, UINT message, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    auto* pane = reinterpret_cast<CaptionPane*>(data);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, &ControlMessageProc, id);
        pane->m_hwnd = nullptr;
        return ::DefSubclassProc(hwnd, message, wParam, lParam);
    }
    return pane->HandleControlMessage(message, wParam, lParam);
}

void CaptionPane::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (!m_initialized || ::IsChild(m_hwnd, ::GetFocus())) return;
    HWND target = m_view.Handle();
    if (event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason)
        target = captionTabStop(m_hwnd, event->reason() == Qt::BacktabFocusReason);
    if (target) ::SetFocus(target);
}

bool CaptionPane::nativeEventFilter(const QByteArray& type, void* message, qintptr* result) {
    if (type != "windows_generic_MSG" || !m_hwnd) return false;
    auto* msg = static_cast<MSG*>(message);
    if (msg->hwnd == m_frame && msg->message == WM_SETTINGCHANGE) {
        ::SendMessageW(m_hwnd, msg->message, msg->wParam, msg->lParam);
    }
    if (msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST ||
        (msg->hwnd != m_hwnd && !::IsChild(m_hwnd, msg->hwnd))) return false;

    // Keep native tab order within this pane and hand focus back to Qt at either end.
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB) {
        const bool backward = ::GetKeyState(VK_SHIFT) < 0;
        const HWND boundary = captionTabStop(m_hwnd, !backward);
        if (boundary && (msg->hwnd == boundary || ::IsChild(boundary, msg->hwnd)) &&
            focusNextPrevChild(!backward)) {
            if (result) *result = 0;
            return true;
        }
    }
    if (!::IsDialogMessageW(m_hwnd, msg)) return false;
    if (result) *result = 0;
    return true;
}

LRESULT CALLBACK CaptionPane::PickButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                                    LPARAM lParam, UINT_PTR subclassId,
                                                    DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<CaptionPane*>(referenceData);
    if (!self) return ::DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message) {
        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd);
            self->BeginWindowPick();
            return 0;

        case WM_MOUSEMOVE:
            if (self->m_windowPickDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->UpdateWindowPick(point);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (self->m_windowPickDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->HidePickOutline();
                self->UpdateWindowPick(point, true);
                self->FinishWindowPick(true);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (self->m_windowPickDrag) {
                ::SetCursor(::LoadCursorW(
                    nullptr, IsPickableState(self->m_windowPickState) ? IDC_CROSS : IDC_NO));
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            if (self->m_windowPickDrag && wParam == VK_ESCAPE) {
                self->FinishWindowPick(false);
                return 0;
            }
            break;

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            if (self->m_windowPickDrag) {
                self->FinishWindowPick(false);
                return 0;
            }
            break;

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, &CaptionPane::PickButtonSubclassProc, subclassId);
            break;

        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK CaptionPane::PickOutlineWndProc(HWND hwnd, UINT message, WPARAM wParam,
                                                LPARAM lParam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND: {
            RECT client{};
            ::GetClientRect(hwnd, &client);
            const HBRUSH red = ::CreateSolidBrush(caption_theme::kPickOutline);
            if (red) {
                ::FillRect(reinterpret_cast<HDC>(wParam), &client, red);
                ::DeleteObject(red);
            }
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            RECT client{};
            ::GetClientRect(hwnd, &client);
            const HBRUSH red = ::CreateSolidBrush(caption_theme::kPickOutline);
            if (red) {
                ::FillRect(dc, &client, red);
                ::DeleteObject(red);
            }
            ::EndPaint(hwnd, &paint);
            return 0;
        }

        default:
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

LRESULT CaptionPane::HandleControlMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SETCURSOR: {
            POINT pt{};
            if (LOWORD(lParam) == HTCLIENT && ::GetCursorPos(&pt) &&
                ::ScreenToClient(m_hwnd, &pt)) {
                if (m_sideSplitterDrag || ::PtInRect(&m_sideSplitterRect, pt)) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
                if (m_splitterDrag || ::PtInRect(&m_splitterRect, pt)) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        }

        case WM_NCHITTEST: {
            if (!m_settings.compactView) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (::ScreenToClient(m_hwnd, &point) && point.y >= 0 &&
                    point.y < Scaled(kTitleBarExtension)) {
                    return HTCAPTION;
                }
            }
            break;
        }

        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION) return ::SendMessageW(m_frame, message, wParam, lParam);
            break;

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (::PtInRect(&m_sideSplitterRect, pt)) {
                m_sideSplitterDrag = true;
                m_sideSplitterGrab = pt.x - m_sideSplitterRect.left;
                ::SetCapture(m_hwnd);
                return 0;
            }
            if (::PtInRect(&m_splitterRect, pt)) {
                m_splitterDrag = true;
                m_splitterGrab = pt.y - m_splitterRect.top;
                ::SetCapture(m_hwnd);
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE:
            if (m_sideSplitterDrag) {
                DragSideSplitterTo(GET_X_LPARAM(lParam) - m_sideSplitterGrab);
                return 0;
            }
            if (m_splitterDrag) {
                DragSplitterTo(GET_Y_LPARAM(lParam) - m_splitterGrab);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (m_sideSplitterDrag || m_splitterDrag) {
                m_sideSplitterDrag = false;
                m_splitterDrag = false;
                ::ReleaseCapture();
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            m_sideSplitterDrag = false;
            m_splitterDrag = false;
            break;

        case WM_SIZE:
            // Native controls use the widget's committed physical size, including on first show.
            Layout();
            break;

        case WM_DPICHANGED_AFTERPARENT: {
            m_dpi = ::GetDpiForWindow(m_hwnd);
            if (m_dpi == 0) m_dpi = 96;
            ApplyControlFont();
            Layout();
            break;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item && item->CtlType == ODT_BUTTON) {
                DrawDarkButton(*item);
                return TRUE;
            }
            if (item && item->CtlType == ODT_STATIC &&
                m_targets.DrawItem(*item, m_buttonBrush, m_buttonPressedBrush)) {
                return TRUE;
            }
            break;
        }

        case WM_MOUSEWHEEL:
            // Forward wheel input to the pane even when it does not have focus.
            if (m_view.Handle()) {
                return ::SendMessageW(m_view.Handle(), WM_MOUSEWHEEL, wParam, lParam);
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_RETURN && m_settings.pressEnter) {
                OnSend();
                return 0;
            }
            break;

        case WM_HOTKEY:
            if (wParam == HOTKEY_SEND) {
                QueueHotkeySend();
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == kActiveWindowFollowTimerId) {
                m_activeWindowFollower.Poll();
                return 0;
            }
            if (wParam == kWindowPickTimerId) {
                if (m_windowPickDrag && !m_windowPickUpdating) {
                    POINT point{};
                    if (::GetCapture() != m_pickWindowButton) {
                        FinishWindowPick(false);
                    } else if (::GetCursorPos(&point)) {
                        UpdateWindowPick(point);
                    }
                }
                return 0;
            }
            if (wParam == kHotkeySendTimerId && m_hotkeySendPending) {
                if (HotkeyChordReleased()) {
                    ::KillTimer(m_hwnd, kHotkeySendTimerId);
                    m_hotkeySendPending = false;
                    OnSend();
                }
                return 0;
            }
            break;

        case WM_APP_CAPTION_UPDATE: {
            auto* update = reinterpret_cast<CaptionUpdate*>(lParam);
            OnCaptionUpdate(update);
            delete update;
            return 0;
        }

        case WM_APP_STATUS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                // Capture-source reconnect notices must not hide the reason
                // a hovered browser target is being rejected.
                if (!m_windowPickDrag) SetStatus(*text);
                delete text;
            }
            return 0;
        }

        case WM_APP_SOURCE_CHANGED:
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return OnCtlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));

        case WM_SETTINGCHANGE:
            if (m_settings.theme == UiTheme::System) ApplyTheme();
            break;

        case WM_CLOSE:
            ::PostMessageW(m_frame, WM_CLOSE, 0, 0);
            return 0;

        case WM_DESTROY:
            // The Qt window owns the message loop. Join the producer while its
            // notification HWND still exists, then release any queued payloads.
            m_engine.Stop();
            DrainPendingPayloads();
            if (m_initialized) m_settings.Save();
            ::KillTimer(m_hwnd, kActiveWindowFollowTimerId);
            m_activeWindowFollower.Stop();
            ::KillTimer(m_hwnd, kWindowPickTimerId);
            if (m_hotkeySendPending) {
                ::KillTimer(m_hwnd, kHotkeySendTimerId);
                m_hotkeySendPending = false;
            }
            if (m_pickOutlineWindow) {
                ::DestroyWindow(m_pickOutlineWindow);
                m_pickOutlineWindow = nullptr;
            }
            if (m_hotkeyRegistered) {
                ::UnregisterHotKey(m_hwnd, HOTKEY_SEND);
                m_hotkeyRegistered = false;
            }
            if (m_controlFont) {
                ::DeleteObject(m_controlFont);
                m_controlFont = nullptr;
            }
            DiscardThemeBrushes();
            break;

        default:
            break;
    }
    return ::DefSubclassProc(m_hwnd, message, wParam, lParam);
}

bool CaptionPane::CreateChildren() {
    if (!CaptionView::RegisterWindowClass(m_instance)) return false;
    if (!m_view.Create(m_hwnd, m_instance, IDC_CAPTION_VIEW)) return false;

    // Both panels are plain backdrops. The bottom one sits under the toolbar
    // controls, so every control below carries WS_CLIPSIBLINGS and Layout()
    // keeps the panels at the bottom of the z-order.
    const auto panel = [&](int id) {
        return ::CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 m_instance, nullptr);
    };
    m_rightPanel = panel(IDC_RIGHT_PANEL);
    m_bottomPanel = panel(IDC_BOTTOM_PANEL);
    if (!m_rightPanel || !m_bottomPanel) return false;

    const auto button = [&](const wchar_t* text, int id) {
        return ::CreateWindowExW(0, WC_BUTTONW, text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                     BS_OWNERDRAW,
                                 0, 0, 10, 10, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };
    const auto check = [&](const wchar_t* text, int id) {
        return ::CreateWindowExW(0, WC_BUTTONW, text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                     BS_AUTOCHECKBOX,
                                 0, 0, 10, 10, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };
    const auto combo = [&](int id) {
        return ::CreateWindowExW(0, WC_COMBOBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                     CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 200, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };

    m_sendButton      = button(L"Send", IDC_BTN_SEND);
    m_pressEnterCheck = check(L"Press Enter", IDC_CHK_PRESS_ENTER);
    // Caption text doubles as the accessible name; DrawDarkButton paints icons.
    m_saveButton        = button(L"Save captions", IDC_BTN_SAVE);
    m_clearButton       = button(L"Clear captions", IDC_BTN_CLEAR);
    m_listenButton      = button(L"Stop caption listening", IDC_BTN_LISTEN);
    m_settingsButton    = button(L"Settings", IDC_BTN_SETTINGS);
    m_logButton         = button(L"Log", IDC_BTN_LOG);
    m_pickWindowButton  = button(L"Pick up window", IDC_BTN_PICK_WINDOW);
    // The first tile is created hidden and kept for the life of the window.
    // Further picks add tiles under it.
    if (!m_targets.Create(m_hwnd, [this] { UpdateWindowTitle(); },
                          [this](const std::wstring& message) { SetStatus(message); })) {
        return false;
    }
    if (m_pickWindowButton &&
        !::SetWindowSubclass(m_pickWindowButton, &CaptionPane::PickButtonSubclassProc,
                             IDC_BTN_PICK_WINDOW, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    m_pickOutlineWindow = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kPickOutlineClassName, L"", WS_POPUP, 0, 0, 0, 0, m_frame, nullptr, m_instance, nullptr);
    if (!m_pickOutlineWindow) return false;

    m_fontCombo    = combo(IDC_CBO_FONT);
    m_sizeCombo    = combo(IDC_CBO_SIZE);
    m_spacingCombo = combo(IDC_CBO_SPACING);

    if (!m_sendButton || !m_saveButton || !m_clearButton || !m_listenButton ||
        !m_settingsButton || !m_logButton || !m_pickWindowButton || !m_fontCombo) {
        return false;
    }

    ::SendMessageW(m_pressEnterCheck, BM_SETCHECK, m_settings.pressEnter ? BST_CHECKED : BST_UNCHECKED, 0);

    PopulateFontCombo();
    PopulateSizeCombo();
    PopulateSpacingCombo();
    ApplyControlFont();
    return true;
}

void CaptionPane::EnsureThemeBrushes() {
    if (!m_windowBrush)
        m_windowBrush = ::CreateSolidBrush(m_darkTheme ? caption_theme::kWindowBg : caption_theme::kLightWindowBg);
    if (!m_panelBrush)
        m_panelBrush = ::CreateSolidBrush(m_darkTheme ? caption_theme::kPanelBg : caption_theme::kLightPanelBg);
    if (!m_buttonBrush)
        m_buttonBrush = ::CreateSolidBrush(m_darkTheme ? caption_theme::kButtonFace : caption_theme::kLightButtonFace);
    if (!m_buttonPressedBrush)
        m_buttonPressedBrush =
            ::CreateSolidBrush(m_darkTheme ? caption_theme::kButtonPressed : caption_theme::kLightButtonPressed);
}

void CaptionPane::DiscardThemeBrushes() {
    if (m_windowBrush) {
        ::DeleteObject(m_windowBrush);
        m_windowBrush = nullptr;
    }
    if (m_panelBrush) {
        ::DeleteObject(m_panelBrush);
        m_panelBrush = nullptr;
    }
    if (m_buttonBrush) {
        ::DeleteObject(m_buttonBrush);
        m_buttonBrush = nullptr;
    }
    if (m_buttonPressedBrush) {
        ::DeleteObject(m_buttonPressedBrush);
        m_buttonPressedBrush = nullptr;
    }
}

void CaptionPane::ApplyTheme() {
    m_darkTheme = EffectiveDarkTheme(m_settings.theme);
    DiscardThemeBrushes();
    EnsureThemeBrushes();

    const HWND explorer[] = {m_pressEnterCheck, m_rightPanel, m_bottomPanel, m_view.Handle()};
    for (HWND control : explorer)
        caption_theme::SetControlTheme(control, m_darkTheme ? L"DarkMode_Explorer" : L"Explorer");
    m_targets.SetDarkTheme(m_darkTheme);

    const HWND combos[] = {m_fontCombo, m_sizeCombo, m_spacingCombo};
    for (HWND control : combos)
        caption_theme::SetControlTheme(control, m_darkTheme ? L"DarkMode_CFD" : L"CFD");

    m_view.SetDarkTheme(m_darkTheme);
    const BOOL darkTitleBar = m_darkTheme ? TRUE : FALSE;
    ::DwmSetWindowAttribute(m_frame, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitleBar,
                            sizeof(darkTitleBar));

    update();
}

LRESULT CaptionPane::OnCtlColor(HDC dc, HWND control) {
    EnsureThemeBrushes();

    // The panels, and the controls that sit on the bottom panel, share its fill.
    const bool onPanel = control == m_rightPanel || control == m_bottomPanel ||
                         control == m_pressEnterCheck;
    const COLORREF text = m_darkTheme ? caption_theme::kTextPrimary : caption_theme::kLightTextPrimary;
    const COLORREF background =
        m_darkTheme ? (onPanel ? caption_theme::kPanelBg : caption_theme::kWindowBg)
                    : (onPanel ? caption_theme::kLightPanelBg : caption_theme::kLightWindowBg);
    const HBRUSH brush = onPanel ? m_panelBrush : m_windowBrush;

    ::SetTextColor(dc, text);
    ::SetBkColor(dc, background);
    ::SetBkMode(dc, OPAQUE);
    return reinterpret_cast<LRESULT>(brush);
}

void CaptionPane::DrawDarkButton(const DRAWITEMSTRUCT& item) const {
    const bool pressed = (item.itemState & ODS_SELECTED) != 0 ||
                         (item.hwndItem == m_logButton && m_logVisible);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    const HBRUSH fill = pressed ? m_buttonPressedBrush : m_buttonBrush;
    ::FillRect(item.hDC, &item.rcItem, fill ? fill : reinterpret_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH)));

    const HPEN border =
        ::CreatePen(PS_SOLID, 1, m_darkTheme ? caption_theme::kButtonBorder : caption_theme::kLightButtonBorder);
    const HGDIOBJ oldPen = ::SelectObject(item.hDC, border);
    const HGDIOBJ oldBrush = ::SelectObject(item.hDC, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    ::SelectObject(item.hDC, oldBrush);
    ::SelectObject(item.hDC, oldPen);
    ::DeleteObject(border);

    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC,
                   m_darkTheme ? (disabled ? caption_theme::kTextSecondary : caption_theme::kTextPrimary)
                               : (disabled ? caption_theme::kLightTextSecondary : caption_theme::kLightTextPrimary));

    RECT contentRc = item.rcItem;
    if (pressed) ::OffsetRect(&contentRc, 1, 1);

    // Right-panel icon buttons use Segoe MDL2 Assets glyphs.
    const wchar_t* iconGlyph = nullptr;
    if (item.hwndItem == m_saveButton) {
        iconGlyph = L"\uE74E";  // Save (disk)
    } else if (item.hwndItem == m_clearButton) {
        iconGlyph = L"\uE74D";  // Delete / clear
    } else if (item.hwndItem == m_listenButton) {
        iconGlyph = m_engine.IsPaused() ? L"\uE768" : L"\uE71A";  // Play / Stop
    } else if (item.hwndItem == m_settingsButton) {
        iconGlyph = L"\uE713";  // Settings (gear)
    } else if (item.hwndItem == m_logButton) {
        iconGlyph = L"\uE7BA";  // List, used as the app log
    } else if (item.hwndItem == m_pickWindowButton) {
        iconGlyph = L"\uE78B";  // Window with up-arrow (pick up / raise window)
    }

    if (iconGlyph) {
        const int iconPx = std::max(Scaled(16), 12);
        HFONT iconFont = ::CreateFontW(-iconPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       L"Segoe MDL2 Assets");
        const HGDIOBJ oldFont = iconFont ? ::SelectObject(item.hDC, iconFont) : nullptr;
        ::DrawTextW(item.hDC, iconGlyph, 1, &contentRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (oldFont) ::SelectObject(item.hDC, oldFont);
        if (iconFont) ::DeleteObject(iconFont);
    } else {
        wchar_t text[128]{};
        ::GetWindowTextW(item.hwndItem, text, 128);
        if (m_controlFont) ::SelectObject(item.hDC, m_controlFont);
        ::DrawTextW(item.hDC, text, -1, &contentRc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focused && !pressed) {
        RECT focusRc = item.rcItem;
        ::InflateRect(&focusRc, -3, -3);
        ::DrawFocusRect(item.hDC, &focusRc);
    }
}

void CaptionPane::ApplyControlFont() {
    if (m_controlFont) {
        ::DeleteObject(m_controlFont);
        m_controlFont = nullptr;
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, m_dpi)) {
        m_controlFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!m_controlFont) {
        m_controlFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    const HWND controls[] = {m_sendButton, m_pressEnterCheck, m_fontCombo, m_sizeCombo,
                             m_spacingCombo};
    for (HWND control : controls) {
        if (control) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_controlFont), TRUE);
    }
    m_targets.SetFont(m_controlFont);
}

void CaptionPane::PopulateFontCombo() {
    std::set<std::wstring> families;
    const HDC dc = ::GetDC(m_hwnd);
    LOGFONTW request{};
    request.lfCharSet = DEFAULT_CHARSET;
    ::EnumFontFamiliesExW(dc, &request, &EnumFontProc, reinterpret_cast<LPARAM>(&families), 0);
    ::ReleaseDC(m_hwnd, dc);

    for (const std::wstring& family : families) {
        ::SendMessageW(m_fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(family.c_str()));
    }

    int index = static_cast<int>(::SendMessageW(m_fontCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(m_settings.fontFamily.c_str())));
    if (index == CB_ERR) {
        index = static_cast<int>(::SendMessageW(m_fontCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(L"Segoe UI")));
    }
    ::SendMessageW(m_fontCombo, CB_SETCURSEL, static_cast<WPARAM>(index == CB_ERR ? 0 : index), 0);
}

void CaptionPane::PopulateSizeCombo() {
    static constexpr int kSizes[] = {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28, 32, 36, 42, 48};
    for (int size : kSizes) {
        const std::wstring text = std::to_wstring(size);
        ::SendMessageW(m_sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    const std::wstring current = std::to_wstring(m_settings.fontSizePt);
    int index = static_cast<int>(::SendMessageW(m_sizeCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(current.c_str())));
    if (index == CB_ERR) index = 4;  // 12 pt
    ::SendMessageW(m_sizeCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

void CaptionPane::PopulateSpacingCombo() {
    static constexpr double kSpacings[] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.75, 2.0, 2.5, 3.0};
    int selected = 3;
    for (int i = 0; i < static_cast<int>(std::size(kSpacings)); ++i) {
        wchar_t text[16];
        ::swprintf_s(text, L"%.2g", kSpacings[i]);
        ::SendMessageW(m_spacingCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        if (std::abs(kSpacings[i] - m_settings.lineSpacing) < 0.001) selected = i;
    }
    ::SendMessageW(m_spacingCombo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

void CaptionPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), widgetColor(m_darkTheme ? caption_theme::kWindowBg
                                                    : caption_theme::kLightWindowBg));
    // Layout stores the grab strips in physical pixels, including on fractional-DPI screens.
    painter.scale(1.0 / devicePixelRatioF(), 1.0 / devicePixelRatioF());
    const QColor line = widgetColor(m_darkTheme ? caption_theme::kSplitterLine
                                               : caption_theme::kLightSplitterLine);
    const int thickness = std::max(Scaled(1), 1);
    if (!::IsRectEmpty(&m_splitterRect)) {
        const RECT& strip = m_splitterRect;
        painter.fillRect(strip.left, (strip.top + strip.bottom - thickness) / 2,
                         strip.right - strip.left, thickness, line);
    }
    if (!::IsRectEmpty(&m_sideSplitterRect)) {
        const RECT& strip = m_sideSplitterRect;
        painter.fillRect((strip.left + strip.right - thickness) / 2, strip.top,
                         thickness, strip.bottom - strip.top, line);
    }
}

// Keeps the bottom panel tall enough for its toolbar row and short enough to
// leave a usable caption pane, whatever the window has been resized to.
int CaptionPane::ClampBottomPanelHeight(int wanted, int clientHeight) const {
    const int minimum = Scaled(kBarHeight);
    const int titleBarExtension = m_settings.compactView ? 0 : Scaled(kTitleBarExtension);
    const int available =
        clientHeight - titleBarExtension - LogBarHeight() - Scaled(kSplitterHeight) -
        Scaled(kMinViewHeight);
    return std::clamp(wanted, minimum, std::max(available, minimum));
}

int CaptionPane::ClampSidePanelWidth(int wanted, int clientWidth) const {
    const int minimum = Scaled(kMinSidePanelWidth);
    const int available =
        clientWidth - Scaled(kSideSplitterWidth) - Scaled(kMinColumnWidth);
    return std::clamp(wanted, minimum, std::max(available, minimum));
}

void CaptionPane::DragSideSplitterTo(int splitterLeft) {
    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int width = Unscaled(ClampSidePanelWidth(splitterLeft, client.right - client.left));
    if (width == m_settings.sidePanelWidth) return;

    m_settings.sidePanelWidth = width;
    Layout();
}

void CaptionPane::DragSplitterTo(int splitterTop) {
    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int clientHeight = client.bottom - client.top;

    const int logTop = std::max(clientHeight - LogBarHeight(), 0);
    const int wanted = logTop - splitterTop - Scaled(kSplitterHeight);
    const int height = Unscaled(ClampBottomPanelHeight(wanted, clientHeight));
    if (height == m_settings.bottomPanelHeight) return;

    m_settings.bottomPanelHeight = height;
    Layout();
}

int CaptionPane::LogBarHeight() const {
    return 0;
}

void CaptionPane::Layout() {
    if (!m_hwnd || !m_sendButton) return;

    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;

    const int pad = Scaled(kPad);
    const int gap = Scaled(kGap);
    const int rowHeight = Scaled(kRowHeight);
    const int logHeight = LogBarHeight();
    const int splitterHeight = Scaled(kSplitterHeight);
    const int titleBarExtension = m_settings.compactView ? 0 : Scaled(kTitleBarExtension);
    const int barHeight = ClampBottomPanelHeight(Scaled(m_settings.bottomPanelHeight), clientHeight);

    // The side panel runs down the left edge, with a drag strip on its right.
    // Everything else stacks inside the column beside it: caption pane,
    // splitter, and bottom panel.
    const int sideSplitterWidth = Scaled(kSideSplitterWidth);
    const int sidePanelWidth = ClampSidePanelWidth(Scaled(m_settings.sidePanelWidth), clientWidth);
    const int columnLeft = sidePanelWidth + sideSplitterWidth;
    const int columnWidth = std::max(clientWidth - columnLeft, 0);
    const int logTop = std::max(clientHeight - logHeight, 0);
    const int barTop = std::max(logTop - barHeight, 0);
    const int viewBottom = std::max(barTop - splitterHeight, titleBarExtension);
    const int viewHeight = std::max(viewBottom - titleBarExtension, 0);

    m_splitterRect = RECT{columnLeft, viewBottom, clientWidth, barTop};
    m_sideSplitterRect =
        RECT{sidePanelWidth, titleBarExtension, columnLeft, clientHeight};

    if (m_view.Handle()) {
        ::SetWindowPos(m_view.Handle(), nullptr, columnLeft, titleBarExtension, columnWidth,
                       viewHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Both panels are backdrops, so keep them under their contents.
    if (m_bottomPanel) {
        ::SetWindowPos(m_bottomPanel, HWND_BOTTOM, columnLeft, barTop, columnWidth,
                       std::max(logTop - barTop, 0), SWP_NOACTIVATE);
    }
    if (m_rightPanel) {
        ::SetWindowPos(m_rightPanel, HWND_BOTTOM, 0, titleBarExtension, sidePanelWidth,
                       std::max(clientHeight - titleBarExtension, 0), SWP_NOACTIVATE);
    }

    // Left panel: picker at the top, then one tile per picked tab, newest last.
    const int buttonSize = Scaled(kSideButtonSize);
    const int buttonLeft = std::max((sidePanelWidth - buttonSize) / 2, 0);
    if (m_pickWindowButton) {
        ::SetWindowPos(m_pickWindowButton, nullptr, buttonLeft, titleBarExtension + pad,
                       buttonSize, buttonSize, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    m_targets.SetBounds({0, titleBarExtension + pad + buttonSize + gap,
                         sidePanelWidth, clientHeight - pad}, m_dpi);
    // The toolbar hugs the foot of the bottom panel. A taller panel then opens
    // space under the splitter rather than carrying the controls up with it, so
    // they stay under the pointer while the caption pane is being resized.
    const int rowTop =
        std::max(logTop - Scaled(kBarBottomPad) - rowHeight, barTop + Scaled(kBarTopPad));

    // Left block: Send, Press Enter, then the requested action-icon order.
    int left = columnLeft + pad;
    ::SetWindowPos(m_sendButton, nullptr, left, rowTop, Scaled(kSendWidth), rowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    left += Scaled(kSendWidth) + gap;
    ::SetWindowPos(m_pressEnterCheck, nullptr, left, rowTop, Scaled(96), rowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    left += Scaled(96) + gap;

    const HWND actionButtons[] = {m_listenButton, m_saveButton, m_clearButton, m_settingsButton,
                                  m_logButton};
    for (HWND control : actionButtons) {
        ::SetWindowPos(control, nullptr, left, rowTop, rowHeight, rowHeight,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        left += rowHeight + gap;
    }

    // Right blocks are laid out from the right edge inwards.
    auto placeRight = [&](HWND control, int& cursor, int top, int width, int height) {
        cursor -= width;
        ::SetWindowPos(control, nullptr, cursor, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        cursor -= gap;
    };

    int cursor = clientWidth - pad;
    placeRight(m_spacingCombo, cursor, rowTop, Scaled(62), rowHeight);
    placeRight(m_sizeCombo, cursor, rowTop, Scaled(62), rowHeight);
    placeRight(m_fontCombo, cursor, rowTop, Scaled(140), rowHeight);

    update();
}

void CaptionPane::UpdateHotkeyRegistration() {
    if (m_hotkeySendPending) {
        ::KillTimer(m_hwnd, kHotkeySendTimerId);
        m_hotkeySendPending = false;
    }
    if (m_hotkeyRegistered) {
        ::UnregisterHotKey(m_hwnd, HOTKEY_SEND);
        m_hotkeyRegistered = false;
    }

    const unsigned key = m_settings.hotkeyVirtualKey;
    if (key != 0) {
        m_hotkeyRegistered =
            ::RegisterHotKey(m_hwnd, HOTKEY_SEND, m_settings.hotkeyModifiers | MOD_NOREPEAT, key) !=
            FALSE;
    }
    UpdateWindowTitle();
    if (!m_hotkeyRegistered) SetStatus(L"The Send hotkey is already in use by another app.");
}

void CaptionPane::QueueHotkeySend() {
    if (m_hotkeySendPending) return;

    // Same OnSend as the button, after two hotkey-only steps. Windows lets
    // this app raise the target only for a moment after the hotkey, so claim
    // the window now; Send also releases any still-held modifiers so Enter
    // submits instead of becoming a newline, the usual Shift-chord miss.
    if (m_view.HasSelection()) m_webInputPicker.RaiseSelectedWindow();

    if (HotkeyChordReleased()) {
        OnSend();
        return;
    }

    m_hotkeySendPending = true;
    if (::SetTimer(m_hwnd, kHotkeySendTimerId, kHotkeySendPollMs, nullptr) == 0) {
        m_hotkeySendPending = false;
        SetStatus(L"Could not schedule the Send hotkey action.");
    }
}

bool CaptionPane::HotkeyChordReleased() const {
    const auto released = [](int virtualKey) {
        return (::GetAsyncKeyState(virtualKey) & 0x8000) == 0;
    };

    if (m_settings.hotkeyVirtualKey != 0 &&
        !released(static_cast<int>(m_settings.hotkeyVirtualKey))) {
        return false;
    }
    const unsigned modifiers = m_settings.hotkeyModifiers;
    if ((modifiers & MOD_SHIFT) != 0 && !released(VK_SHIFT)) return false;
    if ((modifiers & MOD_CONTROL) != 0 && !released(VK_CONTROL)) return false;
    if ((modifiers & MOD_ALT) != 0 && !released(VK_MENU)) return false;
    if ((modifiers & MOD_WIN) != 0 && (!released(VK_LWIN) || !released(VK_RWIN))) return false;
    return true;
}

void CaptionPane::UpdateWindowTitle() {
    std::wstring title = kBaseTitle;
    title += L" (Hotkey - ";
    title += HotkeyName(m_settings.hotkeyModifiers, m_settings.hotkeyVirtualKey);
    title += L") : ";
    title += m_webInputPicker.SelectedWindow()
                 ? (m_webInputPicker.SelectedName().empty() ? L"Untitled target"
                                                            : m_webInputPicker.SelectedName())
                 : L"No target selected";
    setWindowTitle(QString::fromStdWString(title));
}

void CaptionPane::setLogVisible(bool visible) {
    if (m_logVisible == visible) return;
    m_logVisible = visible;
    if (m_logButton) ::InvalidateRect(m_logButton, nullptr, TRUE);
}

void CaptionPane::SetStatus(const std::wstring& text) {
    if (!text.empty()) emit logLine(QString::fromStdWString(text));
}

void CaptionPane::ApplyTypography() {
    m_view.SetTypography(m_settings.fontFamily, m_settings.fontSizePt, m_settings.lineSpacing);
}

void CaptionPane::OnCommand(int controlId, int notifyCode) {
    switch (controlId) {
        case IDC_BTN_SEND:
            OnSend();
            return;

        case IDC_BTN_SAVE:
            OnSave();
            return;

        case IDC_BTN_CLEAR:
            OnClear();
            return;

        case IDC_BTN_LISTEN:
            OnToggleListening();
            return;

        case IDC_BTN_SETTINGS:
            OnSettings();
            return;

        case IDC_BTN_LOG:
            m_logVisible = !m_logVisible;
            if (m_logButton) ::InvalidateRect(m_logButton, nullptr, TRUE);
            emit logToggled(m_logVisible);
            return;

        case IDC_BTN_PICK_WINDOW:
            OnPickWindow();
            return;

        case IDC_CHK_PRESS_ENTER:
            m_settings.pressEnter =
                ::SendMessageW(m_pressEnterCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return;

        case IDC_BTN_COPY:
            // Still used by the caption pane (Ctrl+C / context menu).
            OnCopy();
            return;

        case IDCANCEL:
            // IsDialogMessage turns Escape into this before the caption pane can
            // see the key, so the pane's own handler never runs.
            if (m_targets.CancelRemoval()) return;
            if (m_windowPickDrag) {
                FinishWindowPick(false);
                return;
            }
            m_view.ClearSelection();
            return;

        case IDC_CBO_FONT: {
            if (notifyCode != CBN_SELCHANGE) return;
            const int index = static_cast<int>(::SendMessageW(m_fontCombo, CB_GETCURSEL, 0, 0));
            if (index == CB_ERR) return;
            const int length = static_cast<int>(::SendMessageW(m_fontCombo, CB_GETLBTEXTLEN, index, 0));
            if (length <= 0) return;
            std::wstring family(static_cast<size_t>(length) + 1, L'\0');
            ::SendMessageW(m_fontCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(family.data()));
            family.resize(static_cast<size_t>(length));
            m_settings.fontFamily = family;
            ApplyTypography();
            return;
        }

        case IDC_CBO_SIZE: {
            if (notifyCode != CBN_SELCHANGE) return;
            const int index = static_cast<int>(::SendMessageW(m_sizeCombo, CB_GETCURSEL, 0, 0));
            if (index == CB_ERR) return;
            wchar_t text[16]{};
            ::SendMessageW(m_sizeCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
            m_settings.fontSizePt = _wtoi(text);
            ApplyTypography();
            return;
        }

        case IDC_CBO_SPACING: {
            if (notifyCode != CBN_SELCHANGE) return;
            const int index = static_cast<int>(::SendMessageW(m_spacingCombo, CB_GETCURSEL, 0, 0));
            if (index == CB_ERR) return;
            wchar_t text[16]{};
            ::SendMessageW(m_spacingCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
            m_settings.lineSpacing = _wtof(text);
            ApplyTypography();
            return;
        }

        default:
            return;
    }
}

void CaptionPane::OnSend() {
    if (!m_view.HasSelection()) {
        SetStatus(L"Select caption text before sending.");
        return;
    }
    if (!m_webInputPicker.SelectedWindow()) {
        SetStatus(L"Pick a web tab before sending.");
        return;
    }

    const bool pressEnter =
        m_pressEnterCheck &&
        ::SendMessageW(m_pressEnterCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    m_settings.pressEnter = pressEnter;

    std::wstring detail;
    const auto result = m_webInputPicker.SendText(m_view.SelectedText(), pressEnter, detail);
    if (result == WebInputSendResult::Failed) {
        SetStatus(detail);
        return;
    }

    // A verified insertion completes the sticky tail selection. An unconfirmed
    // Enter also clears it because submission may already have happened; other
    // failures keep it intact so retrying remains convenient.
    m_view.ClearSelection();

    if (result == WebInputSendResult::SubmissionUnconfirmed) {
        SetStatus(detail);
        return;
    }

    std::wstring status =
        result == WebInputSendResult::Submitted ? L"Sent selection to "
                                                : L"Inserted selection in ";
    status += m_webInputPicker.SelectedName();
    status += result == WebInputSendResult::Submitted ? L" and confirmed submission." : L".";
    SetStatus(status);
}

void CaptionPane::OnSave() {
    const std::wstring text = m_view.FullText();
    if (text.empty()) {
        SetStatus(L"There are no captions to save.");
        return;
    }

    const std::filesystem::path folder(m_settings.ResolvedSaveFolder());
    std::error_code directoryError;
    if (!std::filesystem::exists(folder, directoryError)) {
        std::filesystem::create_directories(folder, directoryError);
    }
    if (directoryError || !std::filesystem::is_directory(folder, directoryError)) {
        SetStatus(L"Could not create or access the configured caption save folder.");
        return;
    }

    SYSTEMTIME saveTime{};
    ::GetLocalTime(&saveTime);
    const std::wstring defaultFileName =
        caption_save::BuildFileName(saveTime, m_webInputPicker.SelectedName());

    wchar_t path[32768]{};
    ::wcsncpy_s(path, defaultFileName.c_str(), _TRUNCATE);

    const std::wstring initialFolder = folder.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_frame;
    dialog.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrInitialDir = initialFolder.c_str();
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetSaveFileNameW(&dialog)) return;

    const HANDLE file =
        ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetStatus(L"Could not create the caption file in the selected location.");
        return;
    }

    const std::string utf8 = util::ToUtf8(text);
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool saved = ::WriteFile(file, bom, sizeof(bom), &written, nullptr) != FALSE &&
                 written == sizeof(bom);
    if (saved && !utf8.empty()) {
        saved = ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                            nullptr) != FALSE &&
                written == utf8.size();
    }
    ::CloseHandle(file);

    if (!saved) {
        SetStatus(L"Could not finish writing the caption file.");
        return;
    }

    // A successful save completes this caption batch. Match the Clear button's
    // behavior, but only after the file has been fully written so cancellation
    // or an I/O error can never discard unsaved text.
    DrainPendingPayloads();
    m_view.Clear();
    SetStatus(std::wstring(L"Saved captions to ") + path + L" and cleared the caption pane.");
}

void CaptionPane::OnClear() {
    DrainPendingPayloads();
    m_view.Clear();
    SetStatus(L"Caption pane cleared.");
}

void CaptionPane::OnToggleListening() {
    const bool pause = !m_engine.IsPaused();
    m_engine.SetPaused(pause);
    UpdateListeningButton();

    SetStatus(pause ? L"Caption listening stopped. Existing text is preserved."
                    : L"Caption listening resumed.");
}

void CaptionPane::UpdateListeningButton() {
    if (!m_listenButton) return;
    ::SetWindowTextW(m_listenButton,
                     m_engine.IsPaused() ? L"Resume caption listening"
                                         : L"Stop caption listening");
    ::InvalidateRect(m_listenButton, nullptr, TRUE);
}

void CaptionPane::OnSettings() {
    const CaptionSourcePreference previousSource = m_settings.captionSource;
    if (!ShowSettingsDialog(m_frame, m_instance, m_settings)) return;

    m_settings.Save();
    ApplyTheme();
    UpdateHotkeyRegistration();

    if (previousSource != m_settings.captionSource) {
        m_engine.Stop();
        DrainPendingPayloads();
        m_view.Clear();
        m_engine.Start(m_hwnd, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs,
                       ToSourceChoice(m_settings.captionSource));
    }
    SetStatus(L"Settings saved.");
}

void CaptionPane::OnPickWindow() {
    std::wstring status = L"Drag this button onto a browser tab that contains an editable field.";
    if (m_webInputPicker.SelectedWindow()) {
        status += L" Current tab: ";
        status += m_webInputPicker.SelectedName();
        status += L".";
    }
    SetStatus(status);
}

void CaptionPane::BeginWindowPick() {
    if (m_windowPickDrag || m_windowPickUpdating) return;
    HidePickOutline();
    m_webInputPicker.ResetCandidate();
    m_windowPickState = WebInputPickState::NoWindow;
    m_windowPickDrag = true;
    ::SendMessageW(m_pickWindowButton, BM_SETSTATE, TRUE, 0);
    ::SetCapture(m_pickWindowButton);
    if (::GetCapture() != m_pickWindowButton) {
        FinishWindowPick(false);
        return;
    }
    ::SetTimer(m_hwnd, kWindowPickTimerId, kWindowPickPollMs, nullptr);
    ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
    SetStatus(L"Drag over a browser tab. Release when the cursor changes to a crosshair.");
}

void CaptionPane::UpdateWindowPick(POINT screenPoint, bool forceRefresh) {
    if (!m_windowPickDrag || m_windowPickUpdating) return;
    m_windowPickUpdating = true;
    const auto state = forceRefresh ? m_webInputPicker.Inspect(screenPoint, m_hwnd, true)
                                    : m_webInputPicker.Preview(screenPoint, m_hwnd);
    m_windowPickUpdating = false;
    // Never restore a preview after the drag has been cancelled.
    if (!m_windowPickDrag) {
        m_webInputPicker.ResetCandidate();
        return;
    }
    m_windowPickState = state;
    const bool pickable = IsPickableState(m_windowPickState);
    ::SetCursor(::LoadCursorW(nullptr, pickable ? IDC_CROSS : IDC_NO));
    if (pickable) {
        UpdatePickOutline(m_webInputPicker.CandidateWindow());
    } else {
        HidePickOutline();
    }
    ShowWindowPickStatus(m_windowPickState);
}

void CaptionPane::FinishWindowPick(bool accept) {
    if (!m_windowPickDrag) return;
    // Never commit a previous candidate if release reentered an ongoing query.
    if (m_windowPickUpdating) accept = false;
    m_windowPickDrag = false;
    ::KillTimer(m_hwnd, kWindowPickTimerId);
    HidePickOutline();
    if (::GetCapture() == m_pickWindowButton) ::ReleaseCapture();
    ::SendMessageW(m_pickWindowButton, BM_SETSTATE, FALSE, 0);

    const int pickedBefore = m_webInputPicker.PickedCount();
    if (accept && IsPickableState(m_windowPickState) && m_webInputPicker.CommitCandidate()) {
        const bool added = m_webInputPicker.PickedCount() > pickedBefore;
        m_targets.Refresh();
        UpdateWindowTitle();

        const std::wstring& pickedName = m_webInputPicker.CandidateName().empty()
                                              ? m_webInputPicker.SelectedName()
                                              : m_webInputPicker.CandidateName();
        std::wstring status = added ? L"Added web tab: " : L"Picked web tab: ";
        status += pickedName;
        status += L".";
        SetStatus(status);
    } else if (!accept) {
        SetStatus(L"Window picking cancelled.");
    } else {
        ShowWindowPickStatus(m_windowPickState);
    }

    // Reset invalidates in-flight preview results immediately. UIA interfaces
    // are released later on the worker that owns them.
    if (!m_windowPickUpdating) m_webInputPicker.ResetCandidate();
}

void CaptionPane::ShowWindowPickStatus(WebInputPickState state) {
    switch (state) {
        case WebInputPickState::Checking:
            SetStatus(L"Checking this window for an editable web input...");
            return;
        case WebInputPickState::Valid: {
            std::wstring status = L"Release to pick: ";
            status += m_webInputPicker.CandidateName();
            status += L".";
            SetStatus(status);
            return;
        }
        case WebInputPickState::OwnWindow:
            SetStatus(L"This app cannot be selected.");
            return;
        case WebInputPickState::NoWebDocument:
            SetStatus(m_windowPickDrag
                          ? L"Drop unavailable: this browser does not expose an accessible web input."
                          : L"No accessible web input. Open the chat tab in a supported browser.");
            return;
        case WebInputPickState::NoEditableInput:
            SetStatus(L"Drop unavailable: no enabled editable web input. Open the chat tab "
                      L"and drag onto its message box.");
            return;
        default:
            SetStatus(L"Drop unavailable: point at a visible browser or WebView window.");
            return;
    }
}

void CaptionPane::UpdatePickOutline(HWND window) {
    if (!m_pickOutlineWindow || !window || !::IsWindow(window)) {
        HidePickOutline();
        return;
    }

    RECT bounds{};
    if (FAILED(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds,
                                       sizeof(bounds))) ||
        ::IsRectEmpty(&bounds)) {
        if (!::GetWindowRect(window, &bounds) || ::IsRectEmpty(&bounds)) {
            HidePickOutline();
            return;
        }
    }

    UINT targetDpi = ::GetDpiForWindow(window);
    if (targetDpi == 0) targetDpi = 96;
    const int thickness = std::max(::MulDiv(3, static_cast<int>(targetDpi), 96), 3);
    ::InflateRect(&bounds, thickness, thickness);

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        HidePickOutline();
        return;
    }

    HRGN borderRegion = ::CreateRectRgn(0, 0, width, height);
    if (!borderRegion) {
        HidePickOutline();
        return;
    }
    if (width > thickness * 2 && height > thickness * 2) {
        const HRGN interior =
            ::CreateRectRgn(thickness, thickness, width - thickness, height - thickness);
        if (interior) {
            ::CombineRgn(borderRegion, borderRegion, interior, RGN_DIFF);
            ::DeleteObject(interior);
        }
    }

    // SetWindowRgn takes ownership only when it succeeds.
    if (::SetWindowRgn(m_pickOutlineWindow, borderRegion, FALSE) == 0) {
        ::DeleteObject(borderRegion);
        HidePickOutline();
        return;
    }
    ::SetWindowPos(m_pickOutlineWindow, HWND_TOPMOST, bounds.left, bounds.top, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::RedrawWindow(m_pickOutlineWindow, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

void CaptionPane::HidePickOutline() {
    if (m_pickOutlineWindow && ::IsWindowVisible(m_pickOutlineWindow)) {
        ::ShowWindow(m_pickOutlineWindow, SW_HIDE);
    }
}

void CaptionPane::OnCopy() {
    if (m_view.Empty()) {
        SetStatus(L"Nothing to copy yet.");
        return;
    }
    const bool selectionOnly = m_view.HasSelection();
    const std::wstring text = selectionOnly ? m_view.SelectedText() : m_view.FullText();
    if (CopyTextToClipboard(text)) {
        SetStatus(selectionOnly ? L"Selection copied to the clipboard."
                                : L"Whole transcript copied to the clipboard.");
    } else {
        SetStatus(L"Could not open the clipboard.");
    }
}

void CaptionPane::OnCaptionUpdate(CaptionUpdate* update) {
    if (!update) return;
    m_view.QueueUpdate(update->firstDirtyLine, std::move(update->lines));

    // The capture thread reads far faster than a frame lasts, so by the time we
    // get here there may already be newer updates waiting. Folding them into
    // this same repaint keeps the queue from growing without bound, which would
    // otherwise leave the view drifting steadily further behind the source and
    // only catching up when the speaker pauses.
    MSG queued;
    while (::PeekMessageW(&queued, m_hwnd, WM_APP_CAPTION_UPDATE, WM_APP_CAPTION_UPDATE,
                          PM_REMOVE)) {
        auto* pending = reinterpret_cast<CaptionUpdate*>(queued.lParam);
        if (!pending) continue;
        m_view.QueueUpdate(pending->firstDirtyLine, std::move(pending->lines));
        delete pending;
    }

    m_view.Present();
    MaybeCopyRealtime();
}

void CaptionPane::MaybeCopyRealtime() {
    if (!m_settings.copyRealtime) return;
    if (::GetForegroundWindow() != m_frame) return;
    // Whatever the user just picked out by hand outranks the running transcript.
    if (m_view.HasSelection()) return;

    const ULONGLONG now = ::GetTickCount64();
    if (now - m_lastRealtimeCopyTick < kRealtimeCopyIntervalMs) return;
    m_lastRealtimeCopyTick = now;

    CopyTextToClipboard(m_view.FullText());
}

bool CaptionPane::CopyTextToClipboard(const std::wstring& text) {
    if (!::OpenClipboard(m_hwnd)) return false;

    bool ok = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (const HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (auto* buffer = static_cast<wchar_t*>(::GlobalLock(handle))) {
            std::memcpy(buffer, text.c_str(), bytes);
            ::GlobalUnlock(handle);
            ::EmptyClipboard();
            if (::SetClipboardData(CF_UNICODETEXT, handle)) {
                ok = true;  // clipboard owns the handle now
            } else {
                ::GlobalFree(handle);
            }
        } else {
            ::GlobalFree(handle);
        }
    }
    ::CloseClipboard();
    return ok;
}

void CaptionPane::DrainPendingPayloads() {
    MSG msg;
    while (::PeekMessageW(&msg, m_hwnd, WM_APP_CAPTION_UPDATE, WM_APP_CAPTION_UPDATE, PM_REMOVE)) {
        delete reinterpret_cast<CaptionUpdate*>(msg.lParam);
    }
    while (::PeekMessageW(&msg, m_hwnd, WM_APP_STATUS, WM_APP_STATUS, PM_REMOVE)) {
        delete reinterpret_cast<std::wstring*>(msg.lParam);
    }
}
