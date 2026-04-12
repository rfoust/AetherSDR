#pragma once

#include <QWidget>
#include <QMap>
#include <QSplitter>

class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;

namespace AetherSDR {

class BandStackPanel;
class PanadapterApplet;
class PanDropOverlay;
class PanFloatingWindow;
class SpectrumWidget;

enum class DropZone;

// Vertical stack of N PanadapterApplet instances, each showing an
// independent FFT + waterfall for a different panadapter on the radio.
// Single-pan mode: one applet fills the stack (no visible divider).
// Supports drag-to-rearrange and drag-out-to-float (detach to a separate window).
class PanadapterStack : public QWidget {
    Q_OBJECT

public:
    explicit PanadapterStack(QWidget* parent = nullptr);

    // Add/remove panadapter displays
    PanadapterApplet* addPanadapter(const QString& panId);
    void removePanadapter(const QString& panId);
    void removeAll();  // remove all applets and reset splitter
    void rekey(const QString& oldId, const QString& newId);

    // Layout: rebuild splitter structure for a given layout ID
    // layoutId: "1", "2v", "2h", "2h1", "12h", "2x2"
    // panIds: the pan IDs to place in order (A, B, C, D)
    void applyLayout(const QString& layoutId, const QStringList& panIds);

    // Accessors
    PanadapterApplet* panadapter(const QString& panId) const;
    SpectrumWidget* spectrum(const QString& panId) const;
    int count() const { return m_pans.size(); }
    QList<PanadapterApplet*> allApplets() const { return m_pans.values(); }

    // Active pan (determines which pan the applet column shows controls for)
    QString activePanId() const { return m_activePanId; }
    PanadapterApplet* activeApplet() const;
    SpectrumWidget* activeSpectrum() const;
    void setActivePan(const QString& panId);
    void setSplitterOrientation(Qt::Orientation o) { m_splitter->setOrientation(o); }
    BandStackPanel* bandStackPanel() const { return m_bandStackPanel; }
    void setBandStackVisible(bool visible);
    void equalizeSizes();
    void rearrangeLayout(const QString& layoutId);

    // Float/dock a panadapter (detach to separate window / re-attach)
    void floatPanadapter(const QString& panId, const QPoint& globalPos = {});
    void dockPanadapter(const QString& panId, const QString& targetPanId, DropZone zone);
    void dockPanadapterDefault(const QString& panId);  // dock at bottom of root splitter
    bool isFloating(const QString& panId) const;
    PanFloatingWindow* floatingWindow(const QString& panId) const;

    // Save/restore which pans are floating (persisted as ordinal indices)
    void saveFloatState() const;
    QList<int> savedFloatingIndices() const;
    void clearSavedFloatState();

    // Mark all floating windows as shutting down so Cmd+Q close events
    // are accepted instead of triggering dock.
    void prepareShutdown();

signals:
    void activePanChanged(const QString& panId);
    void panFloated(const QString& panId);
    void panDocked(const QString& panId);

protected:
    void dragEnterEvent(QDragEnterEvent* ev) override;
    void dragMoveEvent(QDragMoveEvent* ev) override;
    void dragLeaveEvent(QDragLeaveEvent* ev) override;
    void dropEvent(QDropEvent* ev) override;

private:
    // Find which applet the local position is over
    PanadapterApplet* appletAt(const QPoint& localPos) const;

    // Insert a widget next to target in its parent splitter, splitting as needed
    void insertBySplit(QWidget* target, QWidget* inserted, DropZone zone);

    // Collapse degenerate splitters (single-child) in the tree
    void collapseSplitters();
    void collapseSplittersRecursive(QSplitter* splitter);

    void hideDropOverlay();

    BandStackPanel* m_bandStackPanel{nullptr};
    QSplitter* m_splitter{nullptr};
    QMap<QString, PanadapterApplet*> m_pans;
    QMap<QString, PanFloatingWindow*> m_floatingWindows;
    QString m_activePanId;

    // Drop overlay shown during drag
    PanDropOverlay* m_dropOverlay{nullptr};
    QString         m_dropTargetPanId;
};

} // namespace AetherSDR
