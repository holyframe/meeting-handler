#pragma once

#include <array>
#include <string>

enum class CaptionSourcePreference {
    WindowsLiveCaptions,
    ChromeLiveCaption,
};

enum class UiTheme {
    Dark,
    Light,
    System,
};

enum class WindowAction : size_t { Pin, PictureInPicture, Crop, Opacity, Count };
inline constexpr size_t windowActionCount = static_cast<size_t>(WindowAction::Count);

struct GlobalShortcut {
    unsigned modifiers = 0;
    unsigned key = 0;
};

struct WindowsTopSettings {
    static constexpr unsigned defaultBorderRgb = 0x999999;
    static constexpr int defaultBorderWidth = 300; // Hundredths of a device-independent pixel.
    static constexpr int minBorderWidth = 50, maxBorderWidth = 2000;
    static constexpr int minOpacity = 10;
    bool highlight = true;
    bool suspendWhenMaximized = false;
    unsigned borderRgb = defaultBorderRgb;
    int borderWidth = defaultBorderWidth;
    bool interactivePreview = true;
    int opacityPercent = 62;
    bool opaqueOnHover = false;
    std::array<GlobalShortcut, windowActionCount> shortcuts{};
};

// Simple INI-backed preferences stored next to the executable.
struct Settings {
    CaptionSourcePreference captionSource = CaptionSourcePreference::WindowsLiveCaptions;
    UiTheme      theme        = UiTheme::Dark;
    std::wstring fontFamily   = L"Segoe UI";
    int          fontSizePt   = 12;
    double       lineSpacing  = 1.3;
    bool         pressEnter   = true;
    bool         copyRealtime = false;
    bool         compactView  = false;
    bool         alwaysOnTop  = false;
    WindowsTopSettings windowsTop;
    bool         speakerVolumeArrowKeys = true;
    // An empty device ID follows the Windows default recording device.
    std::wstring microphoneDeviceId;
    std::wstring microphoneDeviceName;
    unsigned microphoneHotkeyModifiers = 0;
    unsigned microphoneHotkeyVirtualKey = 0;
    std::wstring transcriptPath;   // empty means "captions.txt" beside the exe
    // Folder used by the Save button. Empty means "script" beside the exe.
    std::wstring saveFolder;

    // How often the caption source is re-read, in milliseconds.
    //
    // This is the dominant source of client-side lag, because Windows 11 Live
    // captions accepts a UI Automation change subscription but never actually
    // raises an event, leaving polling as the only mechanism that works. A read
    // costs about 0.4 ms, so a tight interval is affordable.
    int pollIntervalMs = 8;

    // Global shortcut for the Send button.
    unsigned hotkeyModifiers = 0x0004 /*MOD_SHIFT*/;
    unsigned hotkeyVirtualKey = 'Z';

    int windowX = -1, windowY = -1, windowW = 960, windowH = 620;

    // Height of the bottom panel at 96 dpi; the caption pane takes whatever the
    // window has left over, so dragging the splitter changes this.
    int bottomPanelHeight = 32;

    // Width of the left panel at 96 dpi. Dragging its splitter changes this.
    int sidePanelWidth = 72;

    void Load();
    void Save() const;

    static std::wstring FilePath();
    std::wstring ResolvedTranscriptPath() const;
    std::wstring ResolvedSaveFolder() const;
};
