#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

class WebInputPicker;

// Native child controls for the caption pane's selected browser tabs.
// WebInputPicker owns target identity and browser automation.
class CaptionTargetList {
public:
    explicit CaptionTargetList(WebInputPicker& picker) : m_webInputPicker(picker) {}
    CaptionTargetList(const CaptionTargetList&) = delete;
    CaptionTargetList& operator=(const CaptionTargetList&) = delete;
    ~CaptionTargetList();
    bool Create(HWND parent, std::function<void()> selectionChanged,
                std::function<void(const std::wstring&)> statusChanged);
    void SetDarkTheme(bool dark);
    void SetFont(HFONT font);
    void SetBounds(RECT bounds, UINT dpi);
    void Refresh();
    bool DrawItem(const DRAWITEMSTRUCT& item, HBRUSH face, HBRUSH pressed) const;
    bool CancelRemoval();

private:
    static LRESULT CALLBACK TileProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    int Scaled(int value) const;
    int SelectedTargetHeight(int controlWidth, int index) const;
    void LayoutTiles();
    void InvalidateTiles() const;
    int  PickedTileIndex(HWND tile) const;
    bool CreatePickedTile();
    void BindPickedTile(int index);
    void ActivatePickedTile(int index);
    void SelectPickedTile(int index);
    void ClosePickedTile(int index);
    void ReorderPickedTile(int from, int to);
    RECT CloseButtonRect(const RECT& tileClient) const;
    bool InCloseButton(HWND tile, POINT clientPoint) const;
    int  PickedTileIndexAt(POINT screen) const;
    void BeginSelectedIconRemoval(HWND tile);
    void UpdateSelectedIconRemoval(POINT screenPoint);
    void FinishSelectedIconRemoval(bool accept);

    WebInputPicker& m_webInputPicker;
    HWND m_parent = nullptr;
    std::function<void()> m_selectionChanged;
    std::function<void(const std::wstring&)> m_statusChanged;
    RECT m_bounds{};
    UINT m_dpi = 96;
    HFONT m_controlFont = nullptr; // Owned by CaptionPane.
    bool m_darkTheme = true;
    struct PickedTile {
        HWND hwnd = nullptr;
        HWND source = nullptr;
        HICON icon = nullptr;
    };
    // Index 0 keeps a stable window so an empty list can hide it instead of
    // destroying it. Later picks are real windows stacked under that one.
    std::vector<PickedTile> m_pickedTiles;
    int m_removingPickedIndex = -1;
    int m_tilePressIndex = -1;
    POINT m_tilePressPoint{};
    bool m_tileReorder = false;
    int m_closePressIndex = -1;
    int m_closeHoverIndex = -1;
    bool m_selectedIconRemoveDrag = false;
    bool m_selectedIconOutside = false;
};
