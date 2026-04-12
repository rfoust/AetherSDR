#include "PanadapterStack.h"
#include "BandStackPanel.h"
#include "PanadapterApplet.h"
#include "PanDropOverlay.h"
#include "PanFloatingWindow.h"
#include "SpectrumWidget.h"
#include "core/AppSettings.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QWindow>

namespace AetherSDR {

// Force a QRhiWidget to recreate its native surface after a cross-window
// reparent.  QRhiWidget (with WA_NativeWindow on macOS) binds its Metal
// surface to the top-level window; moving to a different top-level leaves
// the surface stale.  Hiding the widget, destroying its native handle, and
// showing it again forces Qt to create a fresh surface in the new window.
static void resetNativeSurface(SpectrumWidget* sw)
{
    if (!sw) {
        return;
    }
#ifdef AETHER_GPU_SPECTRUM
    sw->hide();
    if (QWindow* w = sw->windowHandle()) {
        w->destroy();
    }
    // Re-assert the attribute so Qt creates a new native window on show()
    sw->setAttribute(Qt::WA_NativeWindow);
    sw->show();
    // Defer an update so the new surface gets its first frame
    QTimer::singleShot(50, sw, [sw]() {
        sw->update();
    });
#endif
}

PanadapterStack::PanadapterStack(QWidget* parent)
    : QWidget(parent)
{
    setAcceptDrops(true);

    auto* hbox = new QHBoxLayout(this);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    // Band stack panel (hidden by default, left of panadapter)
    m_bandStackPanel = new BandStackPanel(this);
    m_bandStackPanel->setVisible(false);
    hbox->addWidget(m_bandStackPanel);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(3);
    m_splitter->setChildrenCollapsible(false);
    hbox->addWidget(m_splitter, 1);
}

void PanadapterStack::setBandStackVisible(bool visible)
{
    // Lock the splitter's current width so Qt doesn't redistribute
    // pixels when the band stack panel appears/disappears.
    int splitterW = m_splitter->width();
    m_splitter->setFixedWidth(splitterW);

    m_bandStackPanel->setVisible(visible);

    // Grow/shrink the main window to accommodate the panel
    if (QWidget* win = window()) {
        int delta = 80;  // band stack panel width
        QSize sz = win->size();
        win->resize(sz.width() + (visible ? delta : -delta), sz.height());
    }

    // Release the width lock after the resize settles
    QTimer::singleShot(0, this, [this]() {
        m_splitter->setMinimumWidth(0);
        m_splitter->setMaximumWidth(QWIDGETSIZE_MAX);
    });
}

PanadapterApplet* PanadapterStack::addPanadapter(const QString& panId)
{
    if (m_pans.contains(panId))
        return m_pans[panId];

    auto* applet = new PanadapterApplet(m_splitter);
    applet->setPanId(panId);
    applet->spectrumWidget()->setPanIndex(m_pans.size());
    applet->spectrumWidget()->loadSettings();
    m_splitter->addWidget(applet);

    // Equal stretch for all pans
    const int idx = m_splitter->indexOf(applet);
    m_splitter->setStretchFactor(idx, 1);

    m_pans[panId] = applet;

    // First pan becomes active
    if (m_activePanId.isEmpty())
        setActivePan(panId);

    // Equalize sizes whenever a pan is added
    if (m_pans.size() > 1)
        equalizeSizes();

    return applet;
}

void PanadapterStack::removePanadapter(const QString& panId)
{
    auto* applet = m_pans.take(panId);
    if (!applet) return;

    delete applet;

    // If active was removed, switch to first remaining
    if (m_activePanId == panId) {
        if (!m_pans.isEmpty())
            setActivePan(m_pans.firstKey());
        else
            m_activePanId.clear();
    }
}

void PanadapterStack::rekey(const QString& oldId, const QString& newId)
{
    if (auto* applet = m_pans.take(oldId)) {
        m_pans[newId] = applet;
        if (m_activePanId == oldId)
            m_activePanId = newId;
    }
}

PanadapterApplet* PanadapterStack::panadapter(const QString& panId) const
{
    return m_pans.value(panId, nullptr);
}

SpectrumWidget* PanadapterStack::spectrum(const QString& panId) const
{
    auto* applet = m_pans.value(panId, nullptr);
    return applet ? applet->spectrumWidget() : nullptr;
}

PanadapterApplet* PanadapterStack::activeApplet() const
{
    return m_pans.value(m_activePanId, nullptr);
}

SpectrumWidget* PanadapterStack::activeSpectrum() const
{
    auto* applet = activeApplet();
    return applet ? applet->spectrumWidget() : nullptr;
}

void PanadapterStack::setActivePan(const QString& panId)
{
    if (m_activePanId == panId) return;
    m_activePanId = panId;

    // Visual indicator: update active border via property (no stylesheet churn)
    for (auto it = m_pans.begin(); it != m_pans.end(); ++it) {
        it.value()->setProperty("activePan", it.key() == panId);
        it.value()->update();
    }

    emit activePanChanged(panId);
}

static void equalizeSplitter(QSplitter* splitter)
{
    const int count = splitter->count();
    if (count < 2) return;
    const int total = (splitter->orientation() == Qt::Horizontal)
                        ? splitter->width() : splitter->height();
    const int each = total / count;
    QList<int> sizes;
    for (int i = 0; i < count; ++i) {
        sizes.append(each);
        // Recurse into nested splitters
        if (auto* nested = qobject_cast<QSplitter*>(splitter->widget(i)))
            equalizeSplitter(nested);
    }
    splitter->setSizes(sizes);
}

void PanadapterStack::equalizeSizes()
{
    equalizeSplitter(m_splitter);
}

void PanadapterStack::rearrangeLayout(const QString& layoutId)
{
    // Collect only docked applets — floating ones stay in their windows
    QList<PanadapterApplet*> applets;
    for (auto it = m_pans.constBegin(); it != m_pans.constEnd(); ++it) {
        if (!m_floatingWindows.contains(it.key())) {
            applets.append(it.value());
        }
    }
    if (applets.isEmpty()) return;

    // Remove docked applets from current splitter (don't delete them)
    for (auto* a : applets)
        a->setParent(nullptr);

    // Hide + remove old splitter from layout, defer deletion
    m_splitter->hide();
    layout()->removeWidget(m_splitter);
    m_splitter->deleteLater();
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(3);
    m_splitter->setChildrenCollapsible(false);
    layout()->addWidget(m_splitter);

    if (layoutId == "2h" && applets.size() >= 2) {
        m_splitter->setOrientation(Qt::Horizontal);
        m_splitter->addWidget(applets[0]);
        m_splitter->addWidget(applets[1]);
    }
    else if (layoutId == "2h1" && applets.size() >= 3) {
        // A|B on top, C on bottom
        auto* topSplit = new QSplitter(Qt::Horizontal);
        topSplit->setHandleWidth(3);
        topSplit->setChildrenCollapsible(false);
        topSplit->addWidget(applets[0]);
        topSplit->addWidget(applets[1]);
        m_splitter->addWidget(topSplit);
        m_splitter->addWidget(applets[2]);
    }
    else if (layoutId == "12h" && applets.size() >= 3) {
        // A on top, B|C on bottom
        m_splitter->addWidget(applets[0]);
        auto* botSplit = new QSplitter(Qt::Horizontal);
        botSplit->setHandleWidth(3);
        botSplit->setChildrenCollapsible(false);
        botSplit->addWidget(applets[1]);
        botSplit->addWidget(applets[2]);
        m_splitter->addWidget(botSplit);
    }
    else if (layoutId == "3v" && applets.size() >= 3) {
        // A / B / C vertical stack
        m_splitter->addWidget(applets[0]);
        m_splitter->addWidget(applets[1]);
        m_splitter->addWidget(applets[2]);
    }
    else if (layoutId == "2x2" && applets.size() >= 4) {
        // A|B on top, C|D on bottom
        auto* topSplit = new QSplitter(Qt::Horizontal);
        topSplit->setHandleWidth(3);
        topSplit->setChildrenCollapsible(false);
        topSplit->addWidget(applets[0]);
        topSplit->addWidget(applets[1]);
        m_splitter->addWidget(topSplit);
        auto* botSplit = new QSplitter(Qt::Horizontal);
        botSplit->setHandleWidth(3);
        botSplit->setChildrenCollapsible(false);
        botSplit->addWidget(applets[2]);
        botSplit->addWidget(applets[3]);
        m_splitter->addWidget(botSplit);
    }
    else if (layoutId == "4v" && applets.size() >= 4) {
        // A / B / C / D vertical stack
        m_splitter->addWidget(applets[0]);
        m_splitter->addWidget(applets[1]);
        m_splitter->addWidget(applets[2]);
        m_splitter->addWidget(applets[3]);
    }
    else {
        // Default: vertical stack (2v, 1, or fallback)
        for (auto* a : applets)
            m_splitter->addWidget(a);
    }

    // Defer equalize until the new splitter has been laid out by Qt
    QTimer::singleShot(0, this, [this]() { equalizeSizes(); });
}

void PanadapterStack::removeAll()
{
    // Close floating windows first
    for (auto* fw : m_floatingWindows) {
        fw->takeApplet();  // don't let it emit dockRequested
        delete fw;
    }
    m_floatingWindows.clear();

    qDeleteAll(m_pans);
    m_pans.clear();
    m_activePanId.clear();

    // Delete the old splitter and create a fresh one
    delete m_splitter;
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(3);
    m_splitter->setChildrenCollapsible(false);
    layout()->addWidget(m_splitter);
}

// ── Float / Dock ──────────────────────────────────────────────────────────

void PanadapterStack::floatPanadapter(const QString& panId, const QPoint& globalPos)
{
    PanadapterApplet* applet = m_pans.value(panId, nullptr);
    if (!applet) {
        return;
    }
    if (m_floatingWindows.contains(panId)) {
        return;  // already floating
    }

    // Remove from splitter (don't remove from m_pans — applet stays tracked)
    applet->setParent(nullptr);

    // Collapse any degenerate splitters left behind
    collapseSplitters();

    // Create floating window
    auto* fw = new PanFloatingWindow(applet, nullptr);
    m_floatingWindows[panId] = fw;

    if (!globalPos.isNull()) {
        fw->move(globalPos - QPoint(40, 10));
    }
    fw->restoreWindowGeometry();
    fw->show();
    fw->raise();

    // Force the SpectrumWidget (QRhiWidget) to recreate its GPU surface
    // now that it lives in a different top-level window.
    resetNativeSurface(applet->spectrumWidget());

    connect(fw, &PanFloatingWindow::dockRequested,
            this, &PanadapterStack::dockPanadapterDefault);

    // Re-equalize the remaining docked pans
    QTimer::singleShot(0, this, [this]() { equalizeSizes(); });

    saveFloatState();
    emit panFloated(panId);
}

void PanadapterStack::dockPanadapter(const QString& panId,
                                      const QString& targetPanId,
                                      DropZone zone)
{
    PanFloatingWindow* fw = m_floatingWindows.take(panId);
    PanadapterApplet* applet = nullptr;
    if (fw) {
        applet = fw->takeApplet();
        fw->deleteLater();
    } else {
        // Not floating — it's a rearrange within the stack
        applet = m_pans.value(panId, nullptr);
        if (applet) {
            applet->setParent(nullptr);
            collapseSplitters();
        }
    }
    if (!applet) {
        return;
    }

    PanadapterApplet* target = m_pans.value(targetPanId, nullptr);
    if (!target || zone == DropZone::None) {
        // Fallback: append to root splitter
        m_splitter->addWidget(applet);
        applet->show();
    } else if (zone == DropZone::Center) {
        // Swap positions: find target in its parent splitter, replace
        QSplitter* parentSplitter = qobject_cast<QSplitter*>(target->parentWidget());
        if (parentSplitter) {
            int targetIdx = parentSplitter->indexOf(target);
            // Insert the dragged applet at the target's position
            parentSplitter->insertWidget(targetIdx, applet);
            applet->show();
            // Target stays where it is (shifted by one)
        } else {
            m_splitter->addWidget(applet);
            applet->show();
        }
    } else {
        insertBySplit(target, applet, zone);
        applet->show();
    }

    collapseSplitters();
    QTimer::singleShot(0, this, [this]() { equalizeSizes(); });

    // Force the SpectrumWidget to recreate its GPU surface in the new window
    resetNativeSurface(applet->spectrumWidget());

    saveFloatState();
    emit panDocked(panId);
}

void PanadapterStack::dockPanadapterDefault(const QString& panId)
{
    // Dock at the bottom of the root splitter (simple re-attach)
    PanFloatingWindow* fw = m_floatingWindows.take(panId);
    PanadapterApplet* applet = nullptr;
    if (fw) {
        applet = fw->takeApplet();
        fw->deleteLater();
    }
    if (!applet) {
        return;
    }

    m_splitter->addWidget(applet);
    applet->show();

    // Force the SpectrumWidget to recreate its GPU surface in the main window
    resetNativeSurface(applet->spectrumWidget());

    QTimer::singleShot(0, this, [this]() { equalizeSizes(); });

    saveFloatState();
    emit panDocked(panId);
}

bool PanadapterStack::isFloating(const QString& panId) const
{
    return m_floatingWindows.contains(panId);
}

PanFloatingWindow* PanadapterStack::floatingWindow(const QString& panId) const
{
    return m_floatingWindows.value(panId, nullptr);
}

void PanadapterStack::saveFloatState() const
{
    auto& s = AppSettings::instance();
    // Save ordinal indices of floating pans (pan IDs change between sessions)
    QStringList indices;
    QList<QString> allKeys = m_pans.keys();
    for (auto it = m_floatingWindows.constBegin(); it != m_floatingWindows.constEnd(); ++it) {
        int idx = allKeys.indexOf(it.key());
        if (idx >= 0) {
            indices.append(QString::number(idx));
        }
        it.value()->saveWindowGeometry();
    }
    s.setValue("PanFloatingIndices", indices.join(','));
    s.save();
    qDebug() << "PanadapterStack::saveFloatState: indices =" << indices
             << "keys =" << allKeys;
}

QList<int> PanadapterStack::savedFloatingIndices() const
{
    const QString val = AppSettings::instance()
        .value("PanFloatingIndices", "").toString();
    if (val.isEmpty()) {
        return {};
    }
    QList<int> result;
    for (const QString& s : val.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        int idx = s.toInt(&ok);
        if (ok && idx >= 0) {
            result.append(idx);
        }
    }
    return result;
}

void PanadapterStack::clearSavedFloatState()
{
    AppSettings::instance().setValue("PanFloatingIndices", "");
}

void PanadapterStack::prepareShutdown()
{
    for (auto* fw : m_floatingWindows) {
        fw->setAppShuttingDown();
    }
}

// ── Drag-and-Drop Event Handlers ─────────────────────────────────────────

void PanadapterStack::dragEnterEvent(QDragEnterEvent* ev)
{
    if (ev->mimeData()->hasFormat(PanadapterApplet::kMimeType)) {
        ev->acceptProposedAction();
    }
}

void PanadapterStack::dragMoveEvent(QDragMoveEvent* ev)
{
    if (!ev->mimeData()->hasFormat(PanadapterApplet::kMimeType)) {
        return;
    }
    ev->acceptProposedAction();

    PanadapterApplet* target = appletAt(ev->position().toPoint());
    if (!target) {
        hideDropOverlay();
        return;
    }

    // Don't show overlay on the dragged applet itself
    const QString draggedId = QString::fromUtf8(
        ev->mimeData()->data(PanadapterApplet::kMimeType));
    if (target->panId() == draggedId) {
        hideDropOverlay();
        return;
    }

    // Create or reposition overlay
    if (!m_dropOverlay) {
        m_dropOverlay = new PanDropOverlay(this);
    }

    // Position overlay to cover the target applet
    const QPoint targetTopLeft = target->mapTo(this, QPoint(0, 0));
    m_dropOverlay->setGeometry(targetTopLeft.x(), targetTopLeft.y(),
                                target->width(), target->height());
    m_dropOverlay->raise();
    m_dropOverlay->show();

    // Determine which zone the cursor is in
    const QPoint localInOverlay = ev->position().toPoint() - targetTopLeft;
    DropZone zone = m_dropOverlay->zoneAt(localInOverlay);
    m_dropOverlay->setActiveZone(zone);
    m_dropTargetPanId = target->panId();
}

void PanadapterStack::dragLeaveEvent(QDragLeaveEvent* /*ev*/)
{
    hideDropOverlay();

    // If the drag left the PanadapterStack entirely, float the panadapter.
    // We use a short timer so that spurious leave events (e.g. passing over
    // a splitter handle) don't trigger a float.
    // Note: The actual float is triggered from the QDrag result in PanadapterApplet.
}

void PanadapterStack::dropEvent(QDropEvent* ev)
{
    hideDropOverlay();

    if (!ev->mimeData()->hasFormat(PanadapterApplet::kMimeType)) {
        return;
    }

    const QString draggedId = QString::fromUtf8(
        ev->mimeData()->data(PanadapterApplet::kMimeType));

    PanadapterApplet* target = appletAt(ev->position().toPoint());
    if (!target || target->panId() == draggedId) {
        // Dropped on empty space or self — if floating, dock at bottom
        if (m_floatingWindows.contains(draggedId)) {
            dockPanadapterDefault(draggedId);
        }
        ev->acceptProposedAction();
        return;
    }

    // Determine drop zone
    const QPoint targetTopLeft = target->mapTo(this, QPoint(0, 0));
    const QPoint localInTarget = ev->position().toPoint() - targetTopLeft;

    // Use a temporary overlay calc to get the zone
    PanDropOverlay tempOverlay;
    tempOverlay.resize(target->size());
    DropZone zone = tempOverlay.zoneAt(localInTarget);

    dockPanadapter(draggedId, target->panId(), zone);

    ev->acceptProposedAction();
}

// ── Internal Helpers ─────────────────────────────────────────────────────

PanadapterApplet* PanadapterStack::appletAt(const QPoint& localPos) const
{
    for (auto it = m_pans.constBegin(); it != m_pans.constEnd(); ++it) {
        PanadapterApplet* applet = it.value();
        if (m_floatingWindows.contains(it.key())) {
            continue;  // skip floating applets
        }
        if (!applet->isVisible()) {
            continue;
        }
        const QRect r(applet->mapTo(const_cast<PanadapterStack*>(this), QPoint(0, 0)),
                      applet->size());
        if (r.contains(localPos)) {
            return applet;
        }
    }
    return nullptr;
}

void PanadapterStack::insertBySplit(QWidget* target, QWidget* inserted, DropZone zone)
{
    QSplitter* parentSplitter = qobject_cast<QSplitter*>(target->parentWidget());
    if (!parentSplitter) {
        // Target is directly in root — use root splitter
        parentSplitter = m_splitter;
    }

    const int targetIdx = parentSplitter->indexOf(target);

    // Determine orientation needed for this split
    Qt::Orientation needed = Qt::Vertical;
    if (zone == DropZone::Left || zone == DropZone::Right) {
        needed = Qt::Horizontal;
    }

    // If parent splitter already has the right orientation, just insert adjacent
    if (parentSplitter->orientation() == needed) {
        if (zone == DropZone::Top || zone == DropZone::Left) {
            parentSplitter->insertWidget(targetIdx, inserted);
        } else {
            parentSplitter->insertWidget(targetIdx + 1, inserted);
        }
        return;
    }

    // Need to wrap target + inserted in a new sub-splitter with the needed orientation
    auto* newSplitter = new QSplitter(needed);
    newSplitter->setHandleWidth(3);
    newSplitter->setChildrenCollapsible(false);

    // Replace target with the new splitter in the parent
    parentSplitter->insertWidget(targetIdx, newSplitter);

    // Move target into the new splitter
    if (zone == DropZone::Top || zone == DropZone::Left) {
        newSplitter->addWidget(inserted);
        newSplitter->addWidget(target);
    } else {
        newSplitter->addWidget(target);
        newSplitter->addWidget(inserted);
    }

    // Equalize the new sub-splitter
    const int total = (needed == Qt::Horizontal) ? newSplitter->width() : newSplitter->height();
    newSplitter->setSizes({total / 2, total / 2});
}

void PanadapterStack::collapseSplitters()
{
    collapseSplittersRecursive(m_splitter);
}

void PanadapterStack::collapseSplittersRecursive(QSplitter* splitter)
{
    if (!splitter) {
        return;
    }

    // First recurse into nested splitters
    for (int i = splitter->count() - 1; i >= 0; --i) {
        if (auto* nested = qobject_cast<QSplitter*>(splitter->widget(i))) {
            collapseSplittersRecursive(nested);
        }
    }

    // If this splitter has exactly one child, promote it to the parent
    if (splitter->count() == 1 && splitter != m_splitter) {
        QWidget* child = splitter->widget(0);
        QSplitter* parentSplitter = qobject_cast<QSplitter*>(splitter->parentWidget());
        if (parentSplitter) {
            int idx = parentSplitter->indexOf(splitter);
            child->setParent(nullptr);
            parentSplitter->insertWidget(idx, child);
            splitter->deleteLater();
        }
    }

    // If root splitter has exactly one child that is itself a splitter,
    // promote that splitter's children into root
    if (splitter == m_splitter && splitter->count() == 1) {
        if (auto* onlyChild = qobject_cast<QSplitter*>(splitter->widget(0))) {
            // Move all children of onlyChild into root
            while (onlyChild->count() > 0) {
                QWidget* w = onlyChild->widget(0);
                m_splitter->addWidget(w);
            }
            m_splitter->setOrientation(onlyChild->orientation());
            onlyChild->deleteLater();
        }
    }
}

void PanadapterStack::hideDropOverlay()
{
    if (m_dropOverlay) {
        m_dropOverlay->hide();
    }
    m_dropTargetPanId.clear();
}

void PanadapterStack::applyLayout(const QString& layoutId, const QStringList& panIds)
{
    // Build structure based on layout ID.
    // Each layout adds applets to the correct splitter position.
    // panIds must have at least as many entries as the layout requires.

    if (layoutId == "1" && panIds.size() >= 1) {
        // Single pan — just add to vertical splitter
        addPanadapter(panIds[0]);
    }
    else if (layoutId == "2v" && panIds.size() >= 2) {
        // A / B — vertical stack
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
    }
    else if (layoutId == "2h" && panIds.size() >= 2) {
        // A | B — horizontal split
        // Replace the vertical splitter orientation
        m_splitter->setOrientation(Qt::Horizontal);
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
    }
    else if (layoutId == "2h1" && panIds.size() >= 3) {
        // A|B / C — horizontal top, single bottom
        auto* topSplit = new QSplitter(Qt::Horizontal);
        topSplit->setHandleWidth(3);
        topSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(topSplit);

        auto* a = new PanadapterApplet(topSplit);
        a->setPanId(panIds[0]);
        a->spectrumWidget()->setPanIndex(0);
        a->spectrumWidget()->loadSettings();
        topSplit->addWidget(a);
        m_pans[panIds[0]] = a;

        auto* b = new PanadapterApplet(topSplit);
        b->setPanId(panIds[1]);
        b->spectrumWidget()->setPanIndex(1);
        b->spectrumWidget()->loadSettings();
        topSplit->addWidget(b);
        m_pans[panIds[1]] = b;

        auto* c = addPanadapter(panIds[2]);
        Q_UNUSED(c);

        // Equal row heights
        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);

        if (m_activePanId.isEmpty()) setActivePan(panIds[0]);
    }
    else if (layoutId == "12h" && panIds.size() >= 3) {
        // A / B|C — single top, horizontal bottom
        addPanadapter(panIds[0]);

        auto* botSplit = new QSplitter(Qt::Horizontal);
        botSplit->setHandleWidth(3);
        botSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(botSplit);

        auto* b = new PanadapterApplet(botSplit);
        b->setPanId(panIds[1]);
        b->spectrumWidget()->setPanIndex(1);
        b->spectrumWidget()->loadSettings();
        botSplit->addWidget(b);
        m_pans[panIds[1]] = b;

        auto* c = new PanadapterApplet(botSplit);
        c->setPanId(panIds[2]);
        c->spectrumWidget()->setPanIndex(2);
        c->spectrumWidget()->loadSettings();
        botSplit->addWidget(c);
        m_pans[panIds[2]] = c;

        // Equal row heights
        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);

        if (m_activePanId.isEmpty()) setActivePan(panIds[0]);
    }
    else if (layoutId == "2x2" && panIds.size() >= 4) {
        // A|B / C|D — 2×2 grid
        auto* topSplit = new QSplitter(Qt::Horizontal);
        topSplit->setHandleWidth(3);
        topSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(topSplit);

        auto* a = new PanadapterApplet(topSplit);
        a->setPanId(panIds[0]);
        a->spectrumWidget()->setPanIndex(0);
        a->spectrumWidget()->loadSettings();
        topSplit->addWidget(a);
        m_pans[panIds[0]] = a;

        auto* b = new PanadapterApplet(topSplit);
        b->setPanId(panIds[1]);
        b->spectrumWidget()->setPanIndex(1);
        b->spectrumWidget()->loadSettings();
        topSplit->addWidget(b);
        m_pans[panIds[1]] = b;

        auto* botSplit = new QSplitter(Qt::Horizontal);
        botSplit->setHandleWidth(3);
        botSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(botSplit);

        auto* c = new PanadapterApplet(botSplit);
        c->setPanId(panIds[2]);
        c->spectrumWidget()->setPanIndex(2);
        c->spectrumWidget()->loadSettings();
        botSplit->addWidget(c);
        m_pans[panIds[2]] = c;

        auto* d = new PanadapterApplet(botSplit);
        d->setPanId(panIds[3]);
        d->spectrumWidget()->setPanIndex(3);
        d->spectrumWidget()->loadSettings();
        botSplit->addWidget(d);
        m_pans[panIds[3]] = d;

        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);

        if (m_activePanId.isEmpty()) setActivePan(panIds[0]);
    }
    else if (layoutId == "3v" && panIds.size() >= 3) {
        // A / B / C — vertical stack
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
        addPanadapter(panIds[2]);
    }
    else if (layoutId == "4v" && panIds.size() >= 4) {
        // A / B / C / D — vertical stack
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
        addPanadapter(panIds[2]);
        addPanadapter(panIds[3]);
    }
}

} // namespace AetherSDR
