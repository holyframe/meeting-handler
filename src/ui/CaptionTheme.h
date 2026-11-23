#pragma once

class QPalette;
class QString;
class QWidget;
enum class UiTheme;

namespace caption_theme {
inline constexpr int splitterThickness = 6;
inline constexpr int headerControlHeight = 23;
inline constexpr int headerVerticalMargin = 4;
inline constexpr int paneHeaderHeight = headerControlHeight + 2 * headerVerticalMargin;
QString styleSheet(const QString& resource, const QPalette& palette);
void apply(QWidget* widget, UiTheme theme);
}
