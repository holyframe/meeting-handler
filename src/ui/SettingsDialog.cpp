#include "SettingsDialog.h"
#include "CaptionTheme.h"

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
    HotkeyEdit(const Settings& settings, QWidget* parent)
        : QLineEdit(parent), modifiers(settings.hotkeyModifiers), key(settings.hotkeyVirtualKey) {
        setObjectName("sendHotkey");
        setReadOnly(true);
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
}

bool ShowSettingsDialog(QWidget* owner, Settings& settings) {
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
    auto* startCaptions = new QCheckBox(QObject::tr("Start Windows Live Captions when Meeting Handler opens"), &dialog);
    startCaptions->setObjectName("startWindowsLiveCaptions");
    startCaptions->setChecked(settings.startWindowsLiveCaptions);
    form->addRow(QObject::tr("Startup"), startCaptions);
    auto* theme = new QComboBox(&dialog);
    theme->setObjectName("captionTheme");
    theme->addItems({QObject::tr("Dark"), QObject::tr("Light"), QObject::tr("System")});
    theme->setCurrentIndex(static_cast<int>(settings.theme));
    form->addRow(QObject::tr("Theme"), theme);
    const auto applyTheme = [&] { caption_theme::apply(&dialog, static_cast<UiTheme>(theme->currentIndex())); };
    QObject::connect(theme, &QComboBox::currentIndexChanged, &dialog, applyTheme);
    QObject::connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, &dialog, applyTheme);
    applyTheme();

    auto* hotkey = new HotkeyEdit(settings, &dialog);
    hotkey->setAccessibleName(QObject::tr("Send hotkey"));
    auto* change = new QPushButton(QObject::tr("Change..."), &dialog);
    QObject::connect(change, &QPushButton::clicked, hotkey, [hotkey] { hotkey->setFocus(); });
    auto* hotkeyRow = new QHBoxLayout;
    hotkeyRow->addWidget(hotkey, 1);
    hotkeyRow->addWidget(change);
    form->addRow(QObject::tr("Hotkey"), hotkeyRow);

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
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return false;
    settings.captionSource = source->currentIndex() == 1 ? CaptionSourcePreference::ChromeLiveCaption
                                                        : CaptionSourcePreference::WindowsLiveCaptions;
    settings.startWindowsLiveCaptions = startCaptions->isChecked();
    settings.theme = static_cast<UiTheme>(theme->currentIndex());
    settings.saveFolder = folder->text().toStdWString();
    settings.hotkeyModifiers = hotkey->modifiers;
    settings.hotkeyVirtualKey = hotkey->key;
    return true;
}
