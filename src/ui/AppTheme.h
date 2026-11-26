#pragma once

#include <QColor>

class QIcon;
class QPalette;
class QString;
class QToolButton;
class QWidget;
enum class UiTheme;

namespace app_theme {
inline constexpr int splitterThickness = 6;
inline constexpr int headerControlHeight = 23;
inline constexpr int headerVerticalMargin = 4;
inline constexpr int paneHeaderHeight = headerControlHeight + 2 * headerVerticalMargin;
inline constexpr QColor warningBackground{134, 30, 43};
inline constexpr QColor pickerOutline{255, 0, 0};
QColor positiveText(const QPalette& palette);
QIcon buttonIcon(const QString& resource);
QToolButton* toolbarIconButton(QWidget* parent);
// A preview applies only to the Settings dialog or its folder chooser.
void apply(UiTheme theme, QWidget* preview = nullptr);
}
