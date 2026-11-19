add_library(live_caption STATIC
    src/CaptionPane.cpp
    src/ActiveWindowFollower.cpp
    src/CaptionView.cpp
    src/CaptureEngine.cpp
    src/CaptionMerger.cpp
    src/CaptionSave.cpp
    src/CaptionSource.cpp
    src/Settings.cpp
    src/SettingsDialog.cpp
    src/TranscriptStore.cpp
    src/Util.cpp
    src/WebInputPicker.cpp
)
target_include_directories(live_caption PUBLIC src resources)
target_link_libraries(live_caption PRIVATE
    user32 gdi32 comctl32 d2d1 dwrite ole32 oleaut32 uuid dwmapi shlwapi
    uxtheme advapi32 shell32 comdlg32
)
