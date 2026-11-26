#pragma once

#include <QString>
#include "captions/Settings.h"

class QWidget;
class WindowsAudioLevels;

QString captionHotkeyName(unsigned modifiers, unsigned virtualKey);
bool ShowSettingsDialog(QWidget* owner, Settings& settings, WindowsAudioLevels& audio);
