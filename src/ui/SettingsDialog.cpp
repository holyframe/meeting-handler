#include "SettingsDialog.h"
#include "CaptionTheme.h"
#include "windows/WindowsAudioLevels.h"

#include <windows.h>
#include <QCheckBox>
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
#include <QStyleHints>
#include <QTimer>
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
    // The app releases its two global shortcuts while Settings is open.
    constexpr int probeId = 1;
    if (!::RegisterHotKey(window, probeId, edit->modifiers | MOD_NOREPEAT, edit->key)) return false;
    ::UnregisterHotKey(window, probeId);
    return true;
}
}

bool ShowSettingsDialog(QWidget* owner, Settings& settings, WindowsAudioLevels& audio) {
    QDialog dialog(owner);
    dialog.setObjectName("captionSettings");
    dialog.setWindowTitle(QObject::tr("Settings"));
    auto* form = new QFormLayout(&dialog);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
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
    const auto applyTheme = [&] { caption_theme::apply(&dialog, static_cast<UiTheme>(theme->currentIndex())); };
    QObject::connect(theme, &QComboBox::currentIndexChanged, &dialog, applyTheme);
    QObject::connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, &dialog, applyTheme);
    applyTheme();

    const auto addShortcut = [&](const QString& label, const QString& objectName, unsigned mods, unsigned key) {
        auto* edit = new HotkeyEdit(mods, key, &dialog);
        edit->setObjectName(objectName);
        edit->setAccessibleName(label);
        edit->setToolTip(QObject::tr("Press the keys for a global shortcut."));
        auto* change = new QPushButton(QObject::tr("Change..."), &dialog);
        QObject::connect(change, &QPushButton::clicked, edit, [edit] { edit->setFocus(); });
        auto* clear = new QPushButton(QObject::tr("Clear"), &dialog);
        clear->setObjectName(objectName + "Clear");
        QObject::connect(clear, &QPushButton::clicked, edit, &HotkeyEdit::clearShortcut);
        auto* row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(change);
        row->addWidget(clear);
        form->addRow(label, row);
        return edit;
    };
    auto* hotkey = addShortcut(QObject::tr("Send shortcut"), "sendHotkey",
                               settings.hotkeyModifiers, settings.hotkeyVirtualKey);

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

    auto* micHotkey = addShortcut(QObject::tr("Microphone mute shortcut"), "microphoneHotkey",
                                  settings.microphoneHotkeyModifiers, settings.microphoneHotkeyVirtualKey);
    auto* shortcutError = new QLabel(&dialog);
    shortcutError->setObjectName("shortcutError");
    shortcutError->setWordWrap(true);
    shortcutError->hide();
    form->addRow(shortcutError);
    QObject::connect(hotkey, &QLineEdit::textChanged, shortcutError, &QWidget::hide);
    QObject::connect(micHotkey, &QLineEdit::textChanged, shortcutError, &QWidget::hide);

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
        caption_theme::apply(&chooser, static_cast<UiTheme>(theme->currentIndex()));
        if (chooser.exec() == QDialog::Accepted && !chooser.selectedFiles().isEmpty())
            folder->setText(chooser.selectedFiles().first());
    });
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(browse);
    form->addRow(QObject::tr("Save folder"), folderRow);
    form->addRow(QString(), new QLabel(QObject::tr("Save creates a timestamped file named after the picked tab.")));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto window = reinterpret_cast<HWND>(dialog.winId());
        if (micHotkey->key && micHotkey->key == hotkey->key && micHotkey->modifiers == hotkey->modifiers)
            shortcutError->setText(QObject::tr("Choose different shortcuts for Send and microphone mute."));
        else if (!shortcutAvailable(window, hotkey, settings.hotkeyModifiers, settings.hotkeyVirtualKey))
            shortcutError->setText(QObject::tr("The Send shortcut is unavailable. Choose another shortcut."));
        else if (!shortcutAvailable(window, micHotkey, settings.microphoneHotkeyModifiers, settings.microphoneHotkeyVirtualKey))
            shortcutError->setText(QObject::tr("The microphone mute shortcut is unavailable. Choose another shortcut."));
        else {
            dialog.accept();
            return;
        }
        shortcutError->show();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return false;
    settings.captionSource = source->currentIndex() == 1 ? CaptionSourcePreference::ChromeLiveCaption
                                                        : CaptionSourcePreference::WindowsLiveCaptions;
    settings.theme = static_cast<UiTheme>(theme->currentIndex());
    settings.saveFolder = folder->text().toStdWString();
    settings.hotkeyModifiers = hotkey->modifiers;
    settings.hotkeyVirtualKey = hotkey->key;
    const auto selected = microphone->currentData().value<MicrophoneDevice>();
    settings.microphoneDeviceId = selected.id.toStdWString();
    settings.microphoneDeviceName = selected.name.toStdWString();
    settings.microphoneHotkeyModifiers = micHotkey->modifiers;
    settings.microphoneHotkeyVirtualKey = micHotkey->key;
    settings.speakerVolumeArrowKeys = volumeArrows->isChecked();
    return true;
}
