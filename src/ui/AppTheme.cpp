#include "AppTheme.h"
#include "captions/Settings.h"

#include <windows.h>
#include <dwmapi.h>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPointer>
#include <QSlider>
#include <QStyle>
#include <QStyleHints>
#include <QToolButton>
#include <utility>

namespace {
bool darkPalette(const QPalette& palette) {
    return palette.color(QPalette::Window).lightness() < 128;
}

QPalette themePalette(UiTheme theme) {
    QPalette palette = qApp->style()->standardPalette();
    const auto system = qApp->styleHints()->colorScheme();
    const bool dark = theme == UiTheme::Dark || (theme == UiTheme::System &&
        (system == Qt::ColorScheme::Dark || (system == Qt::ColorScheme::Unknown && darkPalette(palette))));
    const QColor text = dark ? QColor(228, 228, 228) : QColor(32, 32, 32);
    palette.setColor(QPalette::Window, dark ? QColor(32, 32, 32) : Qt::white);
    palette.setColor(QPalette::Base, dark ? QColor(43, 43, 43) : Qt::white);
    palette.setColor(QPalette::AlternateBase, dark ? QColor(36, 36, 36) : QColor(246, 246, 246));
    palette.setColor(QPalette::Button, dark ? QColor(55, 55, 55) : QColor(240, 240, 240));
    palette.setColor(QPalette::Midlight, dark ? QColor(72, 72, 72) : QColor(224, 230, 236));
    palette.setColor(QPalette::Mid, dark ? QColor(90, 90, 90) : QColor(150, 150, 150));
    palette.setColor(QPalette::Highlight, dark ? QColor(88, 88, 88) : QColor(198, 220, 255));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, palette.color(QPalette::Button));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Link, dark ? QColor("#a4c9f0") : QColor("#23598c"));
    palette.setColor(QPalette::PlaceholderText, dark ? QColor("#b0b0b0") : QColor("#595959"));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText, QPalette::HighlightedText}) {
        palette.setColor(role, text);
        palette.setColor(QPalette::Disabled, role, dark ? QColor(155, 155, 155) : QColor(105, 105, 105));
    }
    return palette;
}

QString styleSheet(const QPalette& palette) {
    QFile file(QStringLiteral(":/application.qss"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QString sheet = QString::fromUtf8(file.readAll());
    // Resolve roles for this window so a Settings preview can differ from the application.
    for (const auto& [name, role] : {
             std::pair{"window", QPalette::Window}, {"window-text", QPalette::WindowText},
             {"alternate-base", QPalette::AlternateBase}, {"base", QPalette::Base},
             {"button", QPalette::Button}, {"button-text", QPalette::ButtonText},
             {"highlight", QPalette::Highlight}, {"text", QPalette::Text}, {"mid", QPalette::Mid},
             {"midlight", QPalette::Midlight}, {"bright-text", QPalette::BrightText},
             {"link", QPalette::Link}, {"placeholder-text", QPalette::PlaceholderText}}) {
        sheet.replace(QStringLiteral("palette(%1)").arg(QLatin1String(name)), palette.color(role).name());
    }
    sheet.replace("theme(disabled-text)", palette.color(QPalette::Disabled, QPalette::ButtonText).name());
    sheet.replace("theme(warning-background)", app_theme::warningBackground.name());
    sheet.replace("theme(picker-outline)", app_theme::pickerOutline.name());
    return sheet;
}

void updateControlCursor(QWidget* widget) {
    if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QSlider*>(widget) ||
        qobject_cast<QComboBox*>(widget)) {
        widget->setCursor(widget->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
}

void updateTitleBar(QWidget* widget) {
    if (!widget->windowHandle() || (widget->windowType() != Qt::Window && widget->windowType() != Qt::Dialog)) return;
    const BOOL dark = darkPalette(widget->palette());
    ::DwmSetWindowAttribute(reinterpret_cast<HWND>(widget->winId()),
                            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

class WidgetEvents final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::Polish:
        case QEvent::EnabledChange:
            if (auto* widget = qobject_cast<QWidget*>(watched)) updateControlCursor(widget);
            break;
        case QEvent::Show:
        case QEvent::PaletteChange:
            if (auto* widget = qobject_cast<QWidget*>(watched); widget && widget->isWindow()) updateTitleBar(widget);
            break;
        default: break;
        }
        return false;
    }
};

// Use the SVG as a mask at the requested display scale, including after a theme switch.
class ButtonIconEngine final : public QIconEngine {
public:
    explicit ButtonIconEngine(const QString& resource) : source_(resource) {}
    QIconEngine* clone() const override { return new ButtonIconEngine(*this); }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap mask = source_.pixmap(size, 1.0, QIcon::Normal, state);
        if (mask.isNull()) return {};
        QPainter tint(&mask);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        tint.fillRect(mask.rect(), qApp->palette().color(group, QPalette::ButtonText));
        tint.end();
        return mask;
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const QSize pixels = rect.size() * painter->device()->devicePixelRatioF();
        painter->drawPixmap(rect, pixmap(pixels, mode, state));
    }
private:
    QIcon source_;
};
}

QColor app_theme::positiveText(const QPalette& palette) {
    return darkPalette(palette) ? QColor("#86dca4") : QColor("#206c3a");
}

QIcon app_theme::buttonIcon(const QString& resource) {
    return QIcon(new ButtonIconEngine(resource));
}

QToolButton* app_theme::toolbarIconButton(QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setProperty("toolbarGlyph", true);
    button->setFixedSize(headerControlHeight, headerControlHeight);
    return button;
}

void app_theme::apply(UiTheme theme, QWidget* preview) {
    static QPointer<WidgetEvents> events;
    if (!events) {
        events = new WidgetEvents(qApp);
        qApp->installEventFilter(events);
        for (QWidget* widget : QApplication::allWidgets()) updateControlCursor(widget);
    }
    const QPalette palette = themePalette(theme);
    const QString sheet = styleSheet(palette);
    if (preview) {
        preview->setPalette(palette);
        preview->setStyleSheet(sheet);
    } else {
        qApp->setPalette(palette);
        if (qApp->styleSheet() != sheet) qApp->setStyleSheet(sheet);
    }
}
