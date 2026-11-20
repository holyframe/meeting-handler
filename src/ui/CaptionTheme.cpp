#include "CaptionTheme.h"
#include "captions/Settings.h"

#include <windows.h>
#include <dwmapi.h>
#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyleHints>
#include <QWidget>
#include <utility>

void caption_theme::apply(QWidget* widget, UiTheme theme) {
    const auto system = qApp->styleHints()->colorScheme();
    const bool dark = theme == UiTheme::Dark || (theme == UiTheme::System &&
        (system == Qt::ColorScheme::Dark || (system == Qt::ColorScheme::Unknown &&
                                             qApp->palette().window().color().lightness() < 128)));
    QPalette palette = qApp->palette();
    const QColor text = dark ? QColor(228, 228, 228) : QColor(32, 32, 32);
    palette.setColor(QPalette::Window, dark ? QColor(32, 32, 32) : Qt::white);
    palette.setColor(QPalette::Base, dark ? QColor(43, 43, 43) : Qt::white);
    palette.setColor(QPalette::AlternateBase, dark ? QColor(36, 36, 36) : QColor(246, 246, 246));
    palette.setColor(QPalette::Button, dark ? QColor(55, 55, 55) : QColor(240, 240, 240));
    palette.setColor(QPalette::Mid, dark ? QColor(90, 90, 90) : QColor(150, 150, 150));
    palette.setColor(QPalette::Highlight, dark ? QColor(88, 88, 88) : QColor(198, 220, 255));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText, QPalette::HighlightedText}) {
        palette.setColor(role, text);
        palette.setColor(QPalette::Disabled, role, dark ? QColor(155, 155, 155) : QColor(105, 105, 105));
    }
    static const QString sheet = [] {
        QFile file(":/captions.qss");
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
    }();
    // Qt resolves QSS palette() against the system palette for some child controls.
    // Resolve these roles once so an explicit Light or Dark choice covers new children too.
    QString themed = sheet;
    for (const auto& [name, role] : {
             std::pair{"window", QPalette::Window}, {"window-text", QPalette::WindowText},
             {"alternate-base", QPalette::AlternateBase}, {"base", QPalette::Base},
             {"button", QPalette::Button}, {"button-text", QPalette::ButtonText},
             {"highlight", QPalette::Highlight}, {"text", QPalette::Text}, {"mid", QPalette::Mid}}) {
        themed.replace(QStringLiteral("palette(%1)").arg(QLatin1String(name)), palette.color(role).name());
    }
    widget->setStyleSheet(themed);
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
    const BOOL darkTitle = dark;
    ::DwmSetWindowAttribute(reinterpret_cast<HWND>(widget->window()->winId()),
                            DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));
}
