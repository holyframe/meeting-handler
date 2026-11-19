#include "CaptionTargetList.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "CaptionTheme.h"
#include "windows/WebInputPicker.h"
#include "resource.h"

namespace {
constexpr int kTargetPad = 4;
constexpr int kTargetIcon = 20;
constexpr int kTargetGap = 6;
constexpr int kCloseButton = 16;
constexpr int kReorderSlack = 4;
HICON CopyWindowIcon(HWND window) {
    HICON icon = nullptr;
    const WPARAM sizes[] = {ICON_SMALL2, ICON_SMALL, ICON_BIG};
    for (WPARAM size : sizes) {
        DWORD_PTR result = 0;
        if (::SendMessageTimeoutW(window, WM_GETICON, size, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100,
                                  &result) &&
            result != 0) {
            icon = reinterpret_cast<HICON>(result);
            break;
        }
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICONSM));
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICON));
    }
    if (!icon) icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return icon ? ::CopyIcon(icon) : nullptr;
}

}  // namespace

CaptionTargetList::~CaptionTargetList() {
    for (const PickedTile& tile : m_pickedTiles) {
        if (tile.hwnd) {
            ::RemoveWindowSubclass(tile.hwnd, &TileProc, ::GetDlgCtrlID(tile.hwnd));
            ::DestroyWindow(tile.hwnd);
        }
        if (tile.icon) ::DestroyIcon(tile.icon);
    }
}

bool CaptionTargetList::Create(HWND parent, std::function<void()> selectionChanged,
                               std::function<void(const std::wstring&)> statusChanged) {
    m_parent = parent;
    m_selectionChanged = std::move(selectionChanged);
    m_statusChanged = std::move(statusChanged);
    return CreatePickedTile();
}

void CaptionTargetList::SetDarkTheme(bool dark) {
    m_darkTheme = dark;
    for (const PickedTile& tile : m_pickedTiles)
        caption_theme::SetControlTheme(tile.hwnd, dark ? L"DarkMode_Explorer" : L"Explorer");
}

void CaptionTargetList::SetFont(HFONT font) {
    m_controlFont = font;
    for (const PickedTile& tile : m_pickedTiles)
        ::SendMessageW(tile.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void CaptionTargetList::SetBounds(RECT bounds, UINT dpi) {
    m_bounds = bounds;
    m_dpi = dpi;
    LayoutTiles();
}

int CaptionTargetList::Scaled(int value) const {
    return ::MulDiv(value, static_cast<int>(m_dpi), 96);
}

void CaptionTargetList::InvalidateTiles() const {
    for (const PickedTile& tile : m_pickedTiles)
        if (tile.hwnd) ::InvalidateRect(tile.hwnd, nullptr, TRUE);
}

bool CaptionTargetList::CancelRemoval() {
    if (!m_selectedIconRemoveDrag) return false;
    FinishSelectedIconRemoval(false);
    return true;
}

void CaptionTargetList::LayoutTiles() {
    const int chipPad = Scaled(kTargetPad);
    const int chipWidth = std::max(static_cast<int>(m_bounds.right - m_bounds.left) - chipPad * 2, 0);
    int chipTop = m_bounds.top;
    const int pickedCount = m_webInputPicker.PickedCount();
    for (int index = 0; index < static_cast<int>(m_pickedTiles.size()); ++index) {
        HWND tile = m_pickedTiles[static_cast<size_t>(index)].hwnd;
        if (!tile) continue;
        if (index >= pickedCount) {
            ::ShowWindow(tile, SW_HIDE);
            continue;
        }
        const int available = m_bounds.bottom - chipTop;
        if (chipWidth <= 0 || available < Scaled(kTargetIcon)) {
            ::ShowWindow(tile, SW_HIDE);
            continue;
        }
        const int chipHeight = std::min(SelectedTargetHeight(chipWidth, index), available);
        ::SetWindowPos(tile, nullptr, m_bounds.left + chipPad, chipTop, chipWidth, chipHeight,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(tile, SW_SHOWNOACTIVATE);
        chipTop += chipHeight + Scaled(kTargetGap);
    }
}

int CaptionTargetList::SelectedTargetHeight(int controlWidth, int index) const {
    const int pad = Scaled(kTargetPad);
    const int iconSize = std::max(Scaled(kTargetIcon), 16);
    const int gap = Scaled(kTargetGap);
    const bool hasIcon =
        index >= 0 && index < static_cast<int>(m_pickedTiles.size()) &&
        m_pickedTiles[static_cast<size_t>(index)].icon;
    const int closeSize = std::max(Scaled(kCloseButton), 14);
    const int textWidth = std::max(
        controlWidth - pad - (hasIcon ? iconSize + gap : 0) - gap - closeSize - pad, 1);

    std::wstring title = m_webInputPicker.PickedName(index);
    if (title.empty()) title = L"Untitled target";

    int textHeight = iconSize;
    if (const HDC dc = ::GetDC(m_parent)) {
        const HGDIOBJ previous = m_controlFont ? ::SelectObject(dc, m_controlFont) : nullptr;
        RECT measured{0, 0, textWidth, 0};
        ::DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &measured,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EDITCONTROL);
        textHeight = std::max(static_cast<int>(measured.bottom - measured.top), 1);
        if (previous) ::SelectObject(dc, previous);
        ::ReleaseDC(m_parent, dc);
    }
    return std::max(iconSize, textHeight) + pad * 2;
}

bool CaptionTargetList::DrawItem(const DRAWITEMSTRUCT& item, HBRUSH face, HBRUSH pressed) const {
    const int index = PickedTileIndex(item.hwndItem);
    if (index < 0) return false;

    ::FillRect(item.hDC, &item.rcItem, face ? face : static_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH)));

    const bool removingThis = m_selectedIconRemoveDrag && m_removingPickedIndex == index;
    const bool active = index == m_webInputPicker.ActivePicked();
    const COLORREF textColor = m_darkTheme ? caption_theme::kTextPrimary : caption_theme::kLightTextPrimary;
    const COLORREF border = m_darkTheme ? caption_theme::kButtonBorder : caption_theme::kLightButtonBorder;
    const COLORREF borderColor = removingThis && m_selectedIconOutside ? caption_theme::kPickOutline
                                                                      : (active ? textColor : border);
    const HPEN borderPen = ::CreatePen(PS_SOLID, 1, borderColor);
    const HGDIOBJ oldPen = ::SelectObject(item.hDC, borderPen);
    const HGDIOBJ oldBrush = ::SelectObject(item.hDC, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    ::SelectObject(item.hDC, oldBrush);
    ::SelectObject(item.hDC, oldPen);
    ::DeleteObject(borderPen);

    const int pad = Scaled(kTargetPad);
    const int iconSize = std::max(Scaled(kTargetIcon), 16);
    const int gap = Scaled(kTargetGap);
    const HICON icon = m_pickedTiles[static_cast<size_t>(index)].icon;
    if (icon) {
        ::DrawIconEx(item.hDC, item.rcItem.left + pad, item.rcItem.top + pad, icon, iconSize,
                     iconSize, 0, nullptr, DI_NORMAL);
    }

    const int closeSize = std::max(Scaled(kCloseButton), 14);
    std::wstring title = m_webInputPicker.PickedName(index);
    if (title.empty()) title = L"Untitled target";

    RECT textRc = item.rcItem;
    textRc.left += pad + (icon ? iconSize + gap : 0);
    textRc.top += pad;
    textRc.right -= pad + closeSize + gap;
    textRc.bottom -= pad;
    if (textRc.right > textRc.left) {
        const HGDIOBJ oldFont = m_controlFont ? ::SelectObject(item.hDC, m_controlFont) : nullptr;
        ::SetBkMode(item.hDC, TRANSPARENT);
        ::SetTextColor(item.hDC, textColor);
        ::DrawTextW(item.hDC, title.c_str(), static_cast<int>(title.size()), &textRc,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX);
        if (oldFont) ::SelectObject(item.hDC, oldFont);
    }

    const RECT closeRc = CloseButtonRect(item.rcItem);
    const bool closeHot = m_closeHoverIndex == index || m_closePressIndex == index;
    if (closeHot && pressed) ::FillRect(item.hDC, &closeRc, pressed);

    const int iconPx = std::max(Scaled(12), 10);
    HFONT iconFont = ::CreateFontW(-iconPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    const HGDIOBJ oldIconFont = iconFont ? ::SelectObject(item.hDC, iconFont) : nullptr;
    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC, textColor);
    RECT glyphRc = closeRc;
    ::DrawTextW(item.hDC, L"\uE711", 1, &glyphRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldIconFont) ::SelectObject(item.hDC, oldIconFont);
    if (iconFont) ::DeleteObject(iconFont);
    return true;
}

int CaptionTargetList::PickedTileIndex(HWND tile) const {
    for (int index = 0; index < static_cast<int>(m_pickedTiles.size()); ++index) {
        if (m_pickedTiles[static_cast<size_t>(index)].hwnd == tile) return index;
    }
    return -1;
}

bool CaptionTargetList::CreatePickedTile() {
    const int index = static_cast<int>(m_pickedTiles.size());
    const int controlId = index == 0 ? IDC_SELECTED_WINDOW : 1200 + index;
    HWND tile = ::CreateWindowExW(0, WC_STATICW, L"",
                                  WS_CHILD | WS_CLIPSIBLINGS | SS_OWNERDRAW | SS_NOTIFY | SS_NOPREFIX,
                                  0, 0, 10, 10, m_parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (!tile) return false;
    if (!::SetWindowSubclass(tile, &CaptionTargetList::TileProc,
                             static_cast<UINT_PTR>(controlId), reinterpret_cast<DWORD_PTR>(this))) {
        ::DestroyWindow(tile);
        return false;
    }
    caption_theme::SetControlTheme(tile, m_darkTheme ? L"DarkMode_Explorer" : L"Explorer");
    if (m_controlFont) {
        ::SendMessageW(tile, WM_SETFONT, reinterpret_cast<WPARAM>(m_controlFont), TRUE);
    }
    m_pickedTiles.push_back(PickedTile{tile, nullptr, nullptr});
    return true;
}

void CaptionTargetList::BindPickedTile(int index) {
    if (index < 0 || index >= static_cast<int>(m_pickedTiles.size())) return;
    PickedTile& tile = m_pickedTiles[static_cast<size_t>(index)];
    const HWND source = m_webInputPicker.PickedWindow(index);
    if (tile.source != source) {
        if (tile.icon) {
            ::DestroyIcon(tile.icon);
            tile.icon = nullptr;
        }
        tile.icon = source ? CopyWindowIcon(source) : nullptr;
        tile.source = source;
    }

    std::wstring accessibleName = L"Selected window: ";
    accessibleName += m_webInputPicker.PickedName(index);
    ::SetWindowTextW(tile.hwnd, accessibleName.c_str());
    ::InvalidateRect(tile.hwnd, nullptr, TRUE);
}

void CaptionTargetList::Refresh() {
    const int count = m_webInputPicker.PickedCount();
    while (static_cast<int>(m_pickedTiles.size()) > std::max(count, 1)) {
        // The original tile keeps control id 1018 for the life of the window.
        const int last = static_cast<int>(m_pickedTiles.size()) - 1;
        if (last > 0 &&
            ::GetDlgCtrlID(m_pickedTiles[static_cast<size_t>(last)].hwnd) == IDC_SELECTED_WINDOW) {
            std::swap(m_pickedTiles[static_cast<size_t>(last)],
                      m_pickedTiles[static_cast<size_t>(last - 1)]);
        }
        PickedTile extra = std::move(m_pickedTiles.back());
        m_pickedTiles.pop_back();
        if (extra.icon) ::DestroyIcon(extra.icon);
        if (extra.hwnd && ::GetDlgCtrlID(extra.hwnd) != IDC_SELECTED_WINDOW) {
            ::DestroyWindow(extra.hwnd);
        }
    }
    while (static_cast<int>(m_pickedTiles.size()) < std::max(count, 1) && CreatePickedTile()) {
    }

    for (int index = 0; index < count; ++index) BindPickedTile(index);
    if (count == 0 && !m_pickedTiles.empty()) {
        PickedTile& first = m_pickedTiles.front();
        if (first.icon) {
            ::DestroyIcon(first.icon);
            first.icon = nullptr;
        }
        first.source = nullptr;
        ::SetWindowTextW(first.hwnd, L"");
        ::ShowWindow(first.hwnd, SW_HIDE);
    }
    LayoutTiles();
}

RECT CaptionTargetList::CloseButtonRect(const RECT& tileClient) const {
    const int size = std::max(Scaled(kCloseButton), 14);
    const int pad = Scaled(kTargetPad);
    RECT rect{};
    rect.right = tileClient.right - pad;
    rect.left = rect.right - size;
    const int offset = std::max(static_cast<int>((tileClient.bottom - tileClient.top - size) / 2), pad);
    rect.top = tileClient.top + offset;
    rect.bottom = rect.top + size;
    return rect;
}

bool CaptionTargetList::InCloseButton(HWND tile, POINT clientPoint) const {
    RECT client{};
    if (!tile || !::GetClientRect(tile, &client)) return false;
    const RECT button = CloseButtonRect(client);
    return ::PtInRect(&button, clientPoint) != FALSE;
}

int CaptionTargetList::PickedTileIndexAt(POINT screen) const {
    const int count = m_webInputPicker.PickedCount();
    for (int index = 0; index < count && index < static_cast<int>(m_pickedTiles.size()); ++index) {
        const HWND tile = m_pickedTiles[static_cast<size_t>(index)].hwnd;
        if (!tile || !::IsWindowVisible(tile)) continue;
        RECT rect{};
        if (!::GetWindowRect(tile, &rect)) continue;
        if (::PtInRect(&rect, screen)) return index;
    }
    return -1;
}

void CaptionTargetList::SelectPickedTile(int index) {
    if (!m_webInputPicker.SelectPicked(index)) return;
    m_selectionChanged();
    InvalidateTiles();
}

void CaptionTargetList::ClosePickedTile(int index) {
    if (index < 0 || index >= m_webInputPicker.PickedCount()) return;
    m_webInputPicker.RemovePicked(index);
    Refresh();
    m_selectionChanged();
    m_statusChanged(L"Selected tab removed.");
}

void CaptionTargetList::ReorderPickedTile(int from, int to) {
    const int count = m_webInputPicker.PickedCount();
    if (from == to || from < 0 || to < 0 || from >= count || to >= count) return;
    if (from >= static_cast<int>(m_pickedTiles.size()) ||
        to >= static_cast<int>(m_pickedTiles.size())) {
        return;
    }

    m_webInputPicker.MovePicked(from, to);
    PickedTile moving = std::move(m_pickedTiles[static_cast<size_t>(from)]);
    m_pickedTiles.erase(m_pickedTiles.begin() + from);
    m_pickedTiles.insert(m_pickedTiles.begin() + to, std::move(moving));
    LayoutTiles();
}

void CaptionTargetList::ActivatePickedTile(int index) {
    std::wstring error;
    const bool ok = m_webInputPicker.ActivatePicked(index, error);
    m_selectionChanged();
    InvalidateTiles();
    if (!ok) {
        m_statusChanged(error.empty() ? L"Could not bring the picked tab to the front." : error);
        return;
    }

    std::wstring status = L"Brought picked tab to the front: ";
    status += m_webInputPicker.SelectedName();
    status += L".";
    m_statusChanged(status);
}

void CaptionTargetList::BeginSelectedIconRemoval(HWND tile) {
    const int index = PickedTileIndex(tile);
    if (m_selectedIconRemoveDrag || index < 0 || index >= m_webInputPicker.PickedCount()) return;
    m_selectedIconRemoveDrag = true;
    m_selectedIconOutside = false;
    m_removingPickedIndex = index;
    ::SetCapture(tile);

    POINT point{};
    if (::GetCursorPos(&point)) UpdateSelectedIconRemoval(point);
    m_statusChanged(L"Right-drag this tab outside the app and release to remove it.");
}

void CaptionTargetList::UpdateSelectedIconRemoval(POINT screenPoint) {
    if (!m_selectedIconRemoveDrag) return;
    RECT appBounds{};
    const bool outside =
        !::GetWindowRect(::GetAncestor(m_parent, GA_ROOT), &appBounds) || !::PtInRect(&appBounds, screenPoint);
    if (outside != m_selectedIconOutside) {
        m_selectedIconOutside = outside;
        if (m_removingPickedIndex >= 0 &&
            m_removingPickedIndex < static_cast<int>(m_pickedTiles.size())) {
            ::InvalidateRect(m_pickedTiles[static_cast<size_t>(m_removingPickedIndex)].hwnd, nullptr,
                             TRUE);
        }
        m_statusChanged(outside ? L"Release to remove this tab."
                          : L"Drag outside this app to remove this tab.");
    }
    ::SetCursor(::LoadCursorW(nullptr, outside ? IDC_HAND : IDC_SIZEALL));
}

void CaptionTargetList::FinishSelectedIconRemoval(bool accept) {
    if (!m_selectedIconRemoveDrag) return;
    const int index = m_removingPickedIndex;
    const bool remove = accept && m_selectedIconOutside && index >= 0;
    HWND captured = nullptr;
    if (index >= 0 && index < static_cast<int>(m_pickedTiles.size())) {
        captured = m_pickedTiles[static_cast<size_t>(index)].hwnd;
    }
    m_selectedIconRemoveDrag = false;
    m_selectedIconOutside = false;
    m_removingPickedIndex = -1;
    if (captured && ::GetCapture() == captured) ::ReleaseCapture();

    if (remove) {
        m_webInputPicker.RemovePicked(index);
        Refresh();
        m_selectionChanged();
        m_statusChanged(L"Selected tab removed.");
    } else {
        InvalidateTiles();
        m_statusChanged(L"Tab removal cancelled.");
    }
}

LRESULT CALLBACK CaptionTargetList::TileProc(HWND hwnd, UINT message, WPARAM wParam,
                                            LPARAM lParam, UINT_PTR subclassId,
                                            DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<CaptionTargetList*>(referenceData);
    if (!self) return ::DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message) {
        case WM_LBUTTONDOWN: {
            const int index = self->PickedTileIndex(hwnd);
            if (index < 0 || index >= self->m_webInputPicker.PickedCount()) break;
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (self->InCloseButton(hwnd, pt)) {
                self->m_closePressIndex = index;
                ::SetCapture(hwnd);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            self->m_tilePressIndex = index;
            self->m_tilePressPoint = pt;
            self->m_tileReorder = false;
            ::SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            const int index = self->PickedTileIndex(hwnd);
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            self->m_tilePressIndex = -1;
            self->m_tileReorder = false;
            if (index >= 0 && !self->InCloseButton(hwnd, pt)) {
                self->ActivatePickedTile(index);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (self->m_closePressIndex >= 0) {
                const int index = self->m_closePressIndex;
                const bool close = self->InCloseButton(hwnd, pt) &&
                                   self->PickedTileIndex(hwnd) == index;
                self->m_closePressIndex = -1;
                if (::GetCapture() == hwnd) ::ReleaseCapture();
                if (close) self->ClosePickedTile(index);
                else ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self->m_tilePressIndex >= 0 && !self->m_selectedIconRemoveDrag) {
                const bool reorder = self->m_tileReorder;
                const int index = self->PickedTileIndex(hwnd);
                self->m_tilePressIndex = -1;
                self->m_tileReorder = false;
                if (::GetCapture() == hwnd) ::ReleaseCapture();
                if (!reorder && index >= 0 && !self->InCloseButton(hwnd, pt)) {
                    self->SelectPickedTile(index);
                }
                return 0;
            }
            break;
        }

        case WM_RBUTTONDOWN:
            if (self->PickedTileIndex(hwnd) >= 0 && self->m_webInputPicker.PickedCount() > 0) {
                ::SetFocus(hwnd);
                self->BeginSelectedIconRemoval(hwnd);
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (self->m_selectedIconRemoveDrag) {
                POINT point = pt;
                ::ClientToScreen(hwnd, &point);
                self->UpdateSelectedIconRemoval(point);
                return 0;
            }
            if (self->m_tilePressIndex >= 0 && ::GetCapture() == hwnd) {
                const int slack = self->Scaled(kReorderSlack);
                if (!self->m_tileReorder &&
                    (std::abs(pt.x - self->m_tilePressPoint.x) > slack ||
                     std::abs(pt.y - self->m_tilePressPoint.y) > slack)) {
                    self->m_tileReorder = true;
                }
                if (self->m_tileReorder) {
                    POINT screen = pt;
                    ::ClientToScreen(hwnd, &screen);
                    const int from = self->PickedTileIndex(hwnd);
                    const int target = self->PickedTileIndexAt(screen);
                    if (from >= 0 && target >= 0 && target != from) {
                        self->ReorderPickedTile(from, target);
                    }
                    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                }
                return 0;
            }
            const int index = self->PickedTileIndex(hwnd);
            const int hover = index >= 0 && self->InCloseButton(hwnd, pt) ? index : -1;
            if (hover != self->m_closeHoverIndex) {
                self->m_closeHoverIndex = hover;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }

        case WM_RBUTTONUP:
            if (self->m_selectedIconRemoveDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->UpdateSelectedIconRemoval(point);
                self->FinishSelectedIconRemoval(true);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (self->m_tileReorder) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            if (self->m_selectedIconRemoveDrag) {
                const LPCWSTR cursor = self->m_selectedIconOutside ? IDC_HAND : IDC_SIZEALL;
                ::SetCursor(::LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            if (self->m_closeHoverIndex >= 0 ||
                (self->PickedTileIndex(hwnd) >= 0 && self->m_webInputPicker.PickedCount() > 0)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            if (self->m_selectedIconRemoveDrag && wParam == VK_ESCAPE) {
                self->FinishSelectedIconRemoval(false);
                return 0;
            }
            break;

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            self->m_tilePressIndex = -1;
            self->m_tileReorder = false;
            self->m_closePressIndex = -1;
            if (self->m_selectedIconRemoveDrag) {
                self->FinishSelectedIconRemoval(false);
                return 0;
            }
            break;

        case WM_CONTEXTMENU:
            return 0;

        case WM_NCDESTROY: {
            const int index = self->PickedTileIndex(hwnd);
            if (index >= 0) self->m_pickedTiles[static_cast<size_t>(index)].hwnd = nullptr;
            ::RemoveWindowSubclass(hwnd, &CaptionTargetList::TileProc, subclassId);
            break;
        }

        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}
