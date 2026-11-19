#pragma once

class MainWindow;
class QSettings;
struct Settings;

bool installWorkspace(MainWindow& window, const Settings& captionSettings, QSettings& layout);
