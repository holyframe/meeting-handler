#include "SettingsDialog.h"
#include "AppTheme.h"
#include "windows/WindowsAudioLevels.h"

#include <windows.h>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QStyleHints>
#include <QTimer>
#include <QTabWidget>
#include <QVBoxLayout>

QString captionHotkeyName(unsigned modifiers, unsigned key) {
    std::wstring name;
    auto append = [&name](const wchar_t* part) {
        if (!name.empty()) name += L"+";
        name += part;
    };

    if (modifiers & MOD_CONTROL) append(L"Ctrl");
    if (modifiers & MOD_ALT) append(L"Alt");
    if (modifiers & MOD_SHIFT) append(L"Shift");
    if (modifiers & MOD_WIN) append(L"Win");

    if (key >= 'A' && key <= 'Z') {
        const wchar_t keyName[] = {static_cast<wchar_t>(key), L'\0'};
        append(keyName);
    } else if (key >= '0' && key <= '9') {
        const wchar_t keyName[] = {static_cast<wchar_t>(key), L'\0'};
        append(keyName);
    } else if (key != 0) {
        wchar_t keyName[64]{};
        UINT scanCode = ::MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        switch (key) {
            case VK_INSERT:
            case VK_DELETE:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_DIVIDE:
            case VK_NUMLOCK:
                scanCode |= 0x100;
                break;
        }
        if (::GetKeyNameTextW(static_cast<LONG>(scanCode << 16), keyName,
                              static_cast<int>(std::size(keyName))) > 0) {
            append(keyName);
        } else {
            append(L"Unknown");
        }
    }

    return name.empty() ? QStringLiteral("None") : QString::fromStdWString(name);
}

namespace {
// Store the Windows virtual key from the Qt event so existing INI shortcuts, including
// layout-dependent punctuation and extended keys, can round-trip through RegisterHotKey.
class HotkeyEdit final : public QLineEdit {
public:
    HotkeyEdit(unsigned initialModifiers, unsigned initialKey, QWidget* parent)
        : QLineEdit(parent), modifiers(initialModifiers), key(initialKey) {
        setReadOnly(true);
        setText(captionHotkeyName(modifiers, key));
    }
    void clearShortcut() {
        modifiers = 0;
        key = 0;
        setText(captionHotkeyName(modifiers, key));
    }
    unsigned modifiers;
    unsigned key;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
        case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta:
        case Qt::Key_AltGr: return;
        default: break;
        }
        unsigned vk = event->nativeVirtualKey();
        if (!vk && event->key() >= Qt::Key_0 && event->key() <= Qt::Key_Z)
            vk = static_cast<unsigned>(event->key());
        if (!vk && event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F24)
            vk = VK_F1 + event->key() - Qt::Key_F1;
        if (!vk || vk > 255) return QLineEdit::keyPressEvent(event);
        const auto mods = event->modifiers();
        modifiers = (mods.testFlag(Qt::ShiftModifier) ? MOD_SHIFT : 0) |
                    (mods.testFlag(Qt::ControlModifier) ? MOD_CONTROL : 0) |
                    (mods.testFlag(Qt::AltModifier) ? MOD_ALT : 0) |
                    (mods.testFlag(Qt::MetaModifier) ? MOD_WIN : 0);
        key = vk;
        setText(captionHotkeyName(modifiers, key));
        event->accept();
    }
};

void populateMicrophones(QComboBox* combo, const QList<MicrophoneDevice>& devices, const MicrophoneDevice& selected) {
    combo->clear();
    combo->addItem(QObject::tr("Windows default"), QVariant::fromValue(MicrophoneDevice{}));
    int index = 0;
    for (const auto& device : devices) {
        combo->addItem(device.name, QVariant::fromValue(device));
        if (device.id == selected.id) index = combo->count() - 1;
    }
    if (!selected.id.isEmpty() && index == 0) {
        const QString name = selected.name.isEmpty() ? QObject::tr("Selected microphone") : selected.name;
        combo->addItem(QObject::tr("%1 (unavailable)").arg(name), QVariant::fromValue(selected));
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
}

bool shortcutAvailable(HWND window, const HotkeyEdit* edit, unsigned previousModifiers, unsigned previousKey) {
    if (!edit->key || (edit->key == previousKey && edit->modifiers == previousModifiers)) return true;
    // The app releases its global shortcuts while Settings is open.
    constexpr int probeId = 1;
    if (!::RegisterHotKey(window, probeId, edit->modifiers | MOD_NOREPEAT, edit->key)) return false;
    ::UnregisterHotKey(window, probeId);
    return true;
}

QFormLayout* addFormPage(QTabWidget* tabs, const QString& title) {
    auto* page = new QWidget(tabs);
    auto* form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    tabs->addTab(page, title);
    return form;
}

struct ShortcutBinding {
    HotkeyEdit* edit;
    GlobalShortcut* value;
};

void addShortcut(QFormLayout* form, const QString& label, const QString& name,
                 GlobalShortcut& value, QList<ShortcutBinding>& bindings) {
    auto* page = form->parentWidget();
    auto* edit = new HotkeyEdit(value.modifiers, value.key, page);
    edit->setObjectName(name);
    edit->setAccessibleName(label);
    edit->setToolTip(QObject::tr("Press the keys for a global shortcut."));
    auto* change = new QPushButton(QObject::tr("Change..."), page);
    QObject::connect(change, &QPushButton::clicked, edit, [edit] { edit->setFocus(); });
    auto* clear = new QPushButton(QObject::tr("Clear"), page);
    clear->setObjectName(name + "Clear");
    QObject::connect(clear, &QPushButton::clicked, edit, &HotkeyEdit::clearShortcut);
    auto* row = new QHBoxLayout;
    row->addWidget(edit, 1);
    row->addWidget(change);
    row->addWidget(clear);
    form->addRow(label, row);
    bindings.push_back({edit, &value});
}

void addOption(QFormLayout* form, const QString& text, const char* name, bool& value) {
    auto* box = new QCheckBox(text, form->parentWidget());
    box->setObjectName(name);
    box->setChecked(value);
    QObject::connect(box, &QCheckBox::toggled, box, [&value](bool on) { value = on; });
    form->addRow(box);
}

void addWindowSlider(QFormLayout* form, const QString& label, const char* name, int& value,
                     int minimum, int maximum, int divisor, const QString& unit, int reset = -1) {
    auto* row = new QHBoxLayout;
    auto* slider = new QSlider(Qt::Horizontal, form->parentWidget());
    slider->setObjectName(name);
    slider->setAccessibleName(label);
    slider->setRange(minimum, maximum);
    slider->setValue(value);
    auto* readout = new QLabel(form->parentWidget());
    const auto update = [&value, readout, divisor, unit](int n) {
        value = n;
        readout->setText(QString::number(double(n) / divisor, 'f', divisor == 1 ? 0 : 2) + unit);
    };
    update(value);
    QObject::connect(slider, &QSlider::valueChanged, readout, update);
    row->addWidget(slider, 1);
    row->addWidget(readout);
    if (reset >= 0) {
        auto* button = new QPushButton(QObject::tr("Reset"), form->parentWidget());
        button->setAccessibleName(QObject::tr("Reset border width"));
        QObject::connect(button, &QPushButton::clicked, slider, [slider, reset] { slider->setValue(reset); });
        row->addWidget(button);
    }
    form->addRow(label, row);
}

void addWindowsTop(QTabWidget* tabs, WindowsTopSettings& top, QList<ShortcutBinding>& bindings) {
    auto* pin = addFormPage(tabs, QObject::tr("Always on Top"));
    addShortcut(pin, QObject::tr("Pin or unpin window"), "windowPinHotkey",
                top.shortcuts[size_t(WindowAction::Pin)], bindings);
    addOption(pin, QObject::tr("Highlight pinned windows with a border"), "windowBorderEnabled", top.highlight);
    addOption(pin, QObject::tr("Suspend pinning while a window is maximized"), "suspendPinWhenMaximized",
              top.suspendWhenMaximized);
    auto* colorRow = new QHBoxLayout;
    auto* swatch = new QPushButton(pin->parentWidget());
    swatch->setAccessibleName(QObject::tr("Border color"));
    swatch->setObjectName("windowBorderColor");
    auto setColor = [&top, swatch](QColor color) {
        top.borderRgb = color.rgb() & 0xffffff;
        QPixmap chip(20, 16);
        chip.fill(color);
        swatch->setIcon(QIcon(chip));
        swatch->setText(color.name().toUpper());
    };
    setColor(QColor::fromRgb(top.borderRgb));
    QObject::connect(swatch, &QPushButton::clicked, swatch, [&top, swatch, setColor] {
        QColorDialog chooser(QColor::fromRgb(top.borderRgb), swatch);
        chooser.setOption(QColorDialog::DontUseNativeDialog);
        chooser.setWindowTitle(QObject::tr("Border color"));
        if (chooser.exec() == QDialog::Accepted) setColor(chooser.selectedColor());
    });
    auto* reset = new QPushButton(QObject::tr("Reset"), pin->parentWidget());
    reset->setAccessibleName(QObject::tr("Reset border color"));
    QObject::connect(reset, &QPushButton::clicked, swatch, [setColor] {
        setColor(QColor::fromRgb(WindowsTopSettings::defaultBorderRgb));
    });
    colorRow->addWidget(swatch);
    colorRow->addStretch();
    colorRow->addWidget(reset);
    pin->addRow(QObject::tr("Border color"), colorRow);
    addWindowSlider(pin, QObject::tr("Border width"), "windowBorderWidth", top.borderWidth,
                    top.minBorderWidth, top.maxBorderWidth, 100, QObject::tr(" px"), top.defaultBorderWidth);

    auto* pip = addFormPage(tabs, QObject::tr("PiP && Crop"));
    addShortcut(pip, QObject::tr("Open or close preview"), "windowPipHotkey",
                top.shortcuts[size_t(WindowAction::PictureInPicture)], bindings);
    addShortcut(pip, QObject::tr("Crop preview"), "windowCropHotkey", top.shortcuts[size_t(WindowAction::Crop)], bindings);
    addOption(pip, QObject::tr("Control the source window from the preview"), "interactivePreview", top.interactivePreview);
    auto* help = new QLabel(QObject::tr("Choose Crop in the preview and drag over the area to keep. "
                                        "Choose Show full to see the whole window again."));
    help->setWordWrap(true);
    pip->addRow(help);

    auto* opacity = addFormPage(tabs, QObject::tr("Opacity"));
    addShortcut(opacity, QObject::tr("Toggle window opacity"), "windowOpacityHotkey",
                top.shortcuts[size_t(WindowAction::Opacity)], bindings);
    addWindowSlider(opacity, QObject::tr("Default opacity"), "windowOpacityPercent", top.opacityPercent,
                    top.minOpacity, 100, 1, QObject::tr("%"));
    addOption(opacity, QObject::tr("Make the window fully opaque while the pointer is over it"),
              "windowOpaqueOnHover", top.opaqueOnHover);
}
}

bool ShowSettingsDialog(QWidget* owner, Settings& settings, WindowsAudioLevels& audio) {
    WindowsTopSettings top = settings.windowsTop;
    QDialog dialog(owner);
    dialog.setObjectName("captionSettings");
    dialog.setWindowTitle(QObject::tr("Settings"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* pages = new QTabWidget(&dialog);
    layout->addWidget(pages);
    auto* form = addFormPage(pages, QObject::tr("General"));
    auto* source = new QComboBox(&dialog);
    source->setObjectName("captionSource");
    source->addItems({QObject::tr("Windows 11 Live Captions"), QObject::tr("Chrome Live Caption")});
    source->setCurrentIndex(settings.captionSource == CaptionSourcePreference::ChromeLiveCaption ? 1 : 0);
    form->addRow(QObject::tr("Caption source"), source);
    form->addRow(QString(), new QLabel(QObject::tr("Choose which live-caption window this app reads.")));
    auto* theme = new QComboBox(&dialog);
    theme->setObjectName("captionTheme");
    theme->addItems({QObject::tr("Dark"), QObject::tr("Light"), QObject::tr("System")});
    theme->setCurrentIndex(static_cast<int>(settings.theme));
    form->addRow(QObject::tr("Theme"), theme);
    const auto applyTheme = [&] { app_theme::apply(static_cast<UiTheme>(theme->currentIndex()), &dialog); };
    QObject::connect(theme, &QComboBox::currentIndexChanged, &dialog, applyTheme);
    QObject::connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, &dialog, applyTheme);
    applyTheme();

    QList<ShortcutBinding> bindings;
    GlobalShortcut send{settings.hotkeyModifiers, settings.hotkeyVirtualKey};
    GlobalShortcut mute{settings.microphoneHotkeyModifiers, settings.microphoneHotkeyVirtualKey};
    addShortcut(form, QObject::tr("Send shortcut"), "sendHotkey", send, bindings);

    auto* microphone = new QComboBox(&dialog);
    microphone->setObjectName("microphoneDevice");
    microphone->setAccessibleName(QObject::tr("Microphone"));
    microphone->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    microphone->setMinimumContentsLength(32);
    form->addRow(QObject::tr("Microphone"), microphone);
    populateMicrophones(microphone, {}, {QString::fromStdWString(settings.microphoneDeviceId),
                                         QString::fromStdWString(settings.microphoneDeviceName)});
    QList<MicrophoneDevice> lastDevices;
    QObject::connect(&audio, &WindowsAudioLevels::microphonesListed, &dialog, [&](const QList<MicrophoneDevice>& devices) {
        if (devices == lastDevices) return;
        lastDevices = devices;
        populateMicrophones(microphone, devices, microphone->currentData().value<MicrophoneDevice>());
    });
    QTimer rescan;
    QObject::connect(&rescan, &QTimer::timeout, &audio, &WindowsAudioLevels::listMicrophones);
    rescan.start(2000);
    QMetaObject::invokeMethod(&audio, &WindowsAudioLevels::listMicrophones, Qt::QueuedConnection);

    addShortcut(form, QObject::tr("Microphone mute shortcut"), "microphoneHotkey", mute, bindings);

    auto* volumeArrows = new QCheckBox(QObject::tr("Use Up and Down arrows for speaker volume"), &dialog);
    volumeArrows->setObjectName("speakerVolumeArrowKeys");
    volumeArrows->setToolTip(QObject::tr("Adjust Windows speaker volume while Meeting Handler is focused."));
    volumeArrows->setChecked(settings.speakerVolumeArrowKeys);
    form->addRow(volumeArrows);

    auto* folder = new QLineEdit(QString::fromStdWString(settings.ResolvedSaveFolder()), &dialog);
    folder->setObjectName("saveFolder");
    folder->setAccessibleName(QObject::tr("Save folder"));
    auto* browse = new QPushButton(QObject::tr("Browse..."), &dialog);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        QFileDialog chooser(&dialog);
        chooser.setOption(QFileDialog::DontUseNativeDialog);
        chooser.setFileMode(QFileDialog::Directory);
        chooser.setOption(QFileDialog::ShowDirsOnly);
        chooser.setWindowTitle(QObject::tr("Choose caption save folder"));
        chooser.setDirectory(folder->text());
        app_theme::apply(static_cast<UiTheme>(theme->currentIndex()), &chooser);
        if (chooser.exec() == QDialog::Accepted && !chooser.selectedFiles().isEmpty())
            folder->setText(chooser.selectedFiles().first());
    });
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(browse);
    form->addRow(QObject::tr("Save folder"), folderRow);
    form->addRow(QString(), new QLabel(QObject::tr("Save creates a timestamped file named after the picked tab.")));
    addWindowsTop(pages, top, bindings);
    for (int i = 1; i < pages->count(); ++i)
        pages->setTabToolTip(i, QObject::tr("These global shortcuts act on the foreground window."));
    auto* shortcutError = new QLabel(&dialog);
    shortcutError->setObjectName("shortcutError");
    shortcutError->setWordWrap(true);
    shortcutError->hide();
    layout->addWidget(shortcutError);
    for (const auto& binding : bindings)
        QObject::connect(binding.edit, &QLineEdit::textChanged, shortcutError, &QWidget::hide);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto window = reinterpret_cast<HWND>(dialog.winId());
        for (qsizetype i = 0; i < bindings.size(); ++i) {
            const auto [edit, old] = bindings[i];
            if (!edit->key) continue;
            QString problem;
            for (qsizetype j = 0; j < i; ++j) {
                const auto* other = bindings[j].edit;
                if (edit->key == other->key && edit->modifiers == other->modifiers)
                    problem = QObject::tr("Choose different shortcuts for %1 and %2.")
                                  .arg(other->accessibleName(), edit->accessibleName());
            }
            if (problem.isEmpty() && !shortcutAvailable(window, edit, old->modifiers, old->key))
                problem = QObject::tr("%1 is unavailable. Choose another shortcut.").arg(edit->accessibleName());
            if (problem.isEmpty()) continue;
            pages->setCurrentWidget(edit->parentWidget());
            edit->setFocus();
            shortcutError->setText(problem);
            shortcutError->show();
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return false;
    for (const auto& binding : bindings) *binding.value = {binding.edit->modifiers, binding.edit->key};
    settings.windowsTop = top;
    settings.captionSource = source->currentIndex() == 1 ? CaptionSourcePreference::ChromeLiveCaption
                                                        : CaptionSourcePreference::WindowsLiveCaptions;
    settings.theme = static_cast<UiTheme>(theme->currentIndex());
    settings.saveFolder = folder->text().toStdWString();
    settings.hotkeyModifiers = send.modifiers;
    settings.hotkeyVirtualKey = send.key;
    const auto selected = microphone->currentData().value<MicrophoneDevice>();
    settings.microphoneDeviceId = selected.id.toStdWString();
    settings.microphoneDeviceName = selected.name.toStdWString();
    settings.microphoneHotkeyModifiers = mute.modifiers;
    settings.microphoneHotkeyVirtualKey = mute.key;
    settings.speakerVolumeArrowKeys = volumeArrows->isChecked();
    return true;
}
