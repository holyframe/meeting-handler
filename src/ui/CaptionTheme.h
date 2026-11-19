#pragma once

#include <windows.h>
#include <uxtheme.h>

namespace caption_theme {

// Dark UI palette, aligned with the caption pane.
inline constexpr COLORREF kWindowBg       = RGB(32, 32, 32);
inline constexpr COLORREF kPanelBg        = RGB(36, 36, 36);
inline constexpr COLORREF kButtonFace     = RGB(55, 55, 55);
inline constexpr COLORREF kButtonPressed  = RGB(70, 70, 70);
inline constexpr COLORREF kButtonBorder   = RGB(90, 90, 90);
inline constexpr COLORREF kTextPrimary    = RGB(228, 228, 228);
inline constexpr COLORREF kTextSecondary  = RGB(170, 170, 170);
inline constexpr COLORREF kStatusBg       = RGB(28, 28, 28);
inline constexpr COLORREF kSplitterLine   = RGB(72, 72, 72);
inline constexpr COLORREF kPickOutline    = RGB(255, 0, 0);
inline constexpr COLORREF kLightWindowBg      = RGB(255, 255, 255);
inline constexpr COLORREF kLightPanelBg       = RGB(246, 246, 246);
inline constexpr COLORREF kLightButtonFace    = RGB(240, 240, 240);
inline constexpr COLORREF kLightButtonPressed = RGB(220, 220, 220);
inline constexpr COLORREF kLightButtonBorder  = RGB(150, 150, 150);
inline constexpr COLORREF kLightTextPrimary   = RGB(32, 32, 32);
inline constexpr COLORREF kLightTextSecondary = RGB(96, 96, 96);
inline constexpr COLORREF kLightStatusBg      = RGB(246, 246, 246);
inline constexpr COLORREF kLightSplitterLine  = RGB(205, 205, 205);

inline void SetControlTheme(HWND hwnd, const wchar_t* theme) {
    if (!hwnd) return;
    // Prefer the immersive dark control theme when the OS provides it; fall
    // back to the classic unthemed look so WM_CTLCOLOR* can paint dark.
    if (FAILED(::SetWindowTheme(hwnd, theme, nullptr))) {
        ::SetWindowTheme(hwnd, L"", L"");
    }
}

}  // namespace caption_theme
