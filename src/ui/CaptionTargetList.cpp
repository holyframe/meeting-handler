#include "CaptionTargetList.h"
#include "windows/WebInputPicker.h"

#include <QFrame>
#include <QGridLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kStackedTargetWidth = 136;

HICON CopyWindowIcon(HWND window) {
    HICON icon = nullptr;
    const WPARAM sizes[] = {ICON_SMALL2, ICON_SMALL, ICON_BIG};
    for (WPARAM size : sizes) {
        DWORD_PTR result = 0;
        if (::SendMessageTimeoutW(window, WM_GETICON, size, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100,
                                  &result) &&
            result != 0) {
            icon = reinterpret_cast<HICON>(result);
            break;
        }
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICONSM));
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICON));
    }
    if (!icon) icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return icon ? ::CopyIcon(icon) : nullptr;
}
}

CaptionTargetList::CaptionTargetList(WebInputPicker& picker, QWidget* parent)
    : QWidget(parent), m_picker(picker), m_layout(new QVBoxLayout(this)) {
    setObjectName("captionTargets");
    m_layout->setContentsMargins(4, 0, 4, 0);
    m_layout->setSpacing(6);
    m_layout->setAlignment(Qt::AlignTop);
    Refresh();
}

QFrame* CaptionTargetList::createTile() {
    auto* tile = new QFrame(this);
    tile->setObjectName("captionTargetTile");
    tile->setFocusPolicy(Qt::StrongFocus);
    tile->setCursor(Qt::PointingHandCursor);
    tile->setMouseTracking(true);
    tile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    tile->installEventFilter(this);
    auto* row = new QGridLayout(tile);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(3);
    row->setColumnStretch(1, 1);
    auto* icon = new QLabel(tile);
    icon->setObjectName("targetIcon");
    icon->setFixedSize(20, 20);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(icon, 0, 0, Qt::AlignLeft | Qt::AlignTop);
    auto* name = new QLabel(tile);
    name->setObjectName("targetName");
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    name->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* close = new QToolButton(tile);
    close->setObjectName("closeTarget");
    close->setAccessibleName(tr("Remove target"));
    close->setToolTip(close->accessibleName());
    close->setText(QStringLiteral("×"));
    close->setFixedSize(16, 16);
    row->addWidget(close, 0, 2, Qt::AlignRight | Qt::AlignTop);
    placeName(tile);
    connect(close, &QToolButton::clicked, this, [this, tile] { closeTile(m_tiles.indexOf(tile)); });
    m_layout->addWidget(tile);
    return tile;
}

void CaptionTargetList::placeName(QFrame* tile) {
    auto* grid = static_cast<QGridLayout*>(tile->layout());
    auto* name = tile->findChild<QLabel*>("targetName");
    grid->removeWidget(name);
    if (width() < kStackedTargetWidth) grid->addWidget(name, 1, 0, 1, 3);
    else grid->addWidget(name, 0, 1);
}

void CaptionTargetList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if ((event->oldSize().width() < kStackedTargetWidth) != (width() < kStackedTargetWidth)
        || !event->oldSize().isValid()) {
        for (auto* tile : m_tiles) placeName(tile);
    }
}

void CaptionTargetList::Refresh() {
    const int count = m_picker.PickedCount();
    while (m_tiles.size() < std::max(1, count)) m_tiles.append(createTile());
    while (m_tiles.size() > std::max(1, count)) {
        auto* extra = m_tiles.takeLast();
        if (m_pressed == extra) m_pressed = nullptr;
        if (m_removing == extra) CancelRemoval();
        extra->hide();
        extra->deleteLater();
    }
    for (int i = 0; i < m_tiles.size(); ++i) {
        auto* tile = m_tiles[i];
        tile->setVisible(i < count);
        if (i >= count) continue;
        QString name = QString::fromStdWString(m_picker.PickedName(i));
        if (name.isEmpty()) name = tr("Untitled target");
        tile->findChild<QLabel*>("targetName")->setText(name);
        tile->setAccessibleName(tr("Selected window: %1").arg(name));
        tile->setToolTip(name);
        const auto source = reinterpret_cast<quintptr>(m_picker.PickedWindow(i));
        if (tile->property("iconSource").toULongLong() != source) {
            tile->setProperty("iconSource", qulonglong(source));
            HICON icon = CopyWindowIcon(m_picker.PickedWindow(i));
            const QPixmap image = icon ? QPixmap::fromImage(QImage::fromHICON(icon)) : QPixmap{};
            if (icon) ::DestroyIcon(icon);
            tile->findChild<QLabel*>("targetIcon")->setPixmap(image.scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    markActive();
}

void CaptionTargetList::markActive() {
    for (int i = 0; i < m_tiles.size(); ++i) {
        auto* tile = m_tiles[i];
        tile->setProperty("active", i == m_picker.ActivePicked());
        tile->setProperty("removing", tile == m_removing && m_outside);
        tile->style()->unpolish(tile);
        tile->style()->polish(tile);
        tile->update();
    }
}

void CaptionTargetList::closeTile(int index) {
    if (index < 0 || index >= m_picker.PickedCount()) return;
    m_picker.RemovePicked(index);
    Refresh();
    emit selectionChanged();
    emit statusChanged(tr("Selected tab removed."));
}

bool CaptionTargetList::CancelRemoval() {
    if (!m_removing) return false;
    auto* tile = m_removing;
    m_removing = nullptr;
    m_outside = false;
    if (mouseGrabber() == tile) tile->releaseMouse();
    tile->setCursor(Qt::PointingHandCursor);
    markActive();
    emit statusChanged(tr("Tab removal cancelled."));
    return true;
}

void CaptionTargetList::updateRemoval(QFrame* tile, const QPoint& screenPoint) {
    const bool outside = !window()->frameGeometry().contains(screenPoint);
    if (m_outside != outside) {
        m_outside = outside;
        markActive();
        emit statusChanged(outside ? tr("Release to remove this tab.") : tr("Drag outside this app to remove this tab."));
    }
    tile->setCursor(outside ? Qt::PointingHandCursor : Qt::SizeAllCursor);
}

bool CaptionTargetList::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::UngrabMouse:
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: break;
    default: return false;
    }
    auto* tile = qobject_cast<QFrame*>(watched);
    const int index = static_cast<int>(m_tiles.indexOf(tile));
    if (index < 0 || index >= m_picker.PickedCount()) return false;
    if (event->type() == QEvent::UngrabMouse) {
        m_pressed = nullptr;
        CancelRemoval();
        return false;
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) return CancelRemoval();
        if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return) {
            if (m_picker.SelectPicked(index)) { markActive(); emit selectionChanged(); }
            return true;
        }
        return false;
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPoint global = mouse->globalPosition().toPoint();
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        tile->setFocus(Qt::MouseFocusReason);
        if (mouse->button() == Qt::RightButton) {
            m_removing = tile;
            m_outside = false;
            tile->grabMouse();
            emit statusChanged(tr("Right-drag this tab outside the app and release to remove it."));
            updateRemoval(tile, global);
        } else if (mouse->button() == Qt::LeftButton) {
            m_pressed = tile;
            m_pressPoint = global;
            m_reordered = false;
        }
        return true;
    case QEvent::MouseMove:
        if (m_removing == tile) updateRemoval(tile, global);
        else if (m_pressed == tile && (m_reordered || (global - m_pressPoint).manhattanLength() > 4)) {
            m_reordered = true;
            tile->setCursor(Qt::SizeAllCursor);
            for (int to = 0; to < m_picker.PickedCount(); ++to) {
                if (to == index || !m_tiles[to]->rect().contains(m_tiles[to]->mapFromGlobal(global))) continue;
                m_picker.MovePicked(index, to);
                m_tiles.move(index, to);
                m_layout->removeWidget(tile);
                m_layout->insertWidget(to, tile);
                break;
            }
        }
        return true;
    case QEvent::MouseButtonRelease:
        if (mouse->button() == Qt::RightButton && m_removing == tile) {
            updateRemoval(tile, global);
            if (!m_outside) return CancelRemoval();
            m_removing = nullptr;
            m_outside = false;
            if (mouseGrabber() == tile) tile->releaseMouse();
            tile->setCursor(Qt::PointingHandCursor);
            closeTile(index);
        } else if (mouse->button() == Qt::LeftButton && m_pressed == tile) {
            m_pressed = nullptr;
            tile->setCursor(Qt::PointingHandCursor);
            if (!m_reordered && m_picker.SelectPicked(index)) { markActive(); emit selectionChanged(); }
        }
        return true;
    case QEvent::MouseButtonDblClick:
        if (mouse->button() == Qt::LeftButton) {
            m_pressed = nullptr;
            std::wstring error;
            const bool ok = m_picker.ActivatePicked(index, error);
            markActive();
            emit selectionChanged();
            emit statusChanged(ok ? tr("Brought picked tab to the front: %1.").arg(QString::fromStdWString(m_picker.SelectedName()))
                                  : (error.empty() ? tr("Could not bring the picked tab to the front.") : QString::fromStdWString(error)));
        }
        return true;
    default: return false;
    }
}
