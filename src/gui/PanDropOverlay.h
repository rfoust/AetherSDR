#pragma once

#include <QWidget>

namespace AetherSDR {

// Identifies which drop zone the cursor is hovering over.
enum class DropZone {
    None,
    Top,
    Bottom,
    Left,
    Right,
    Center
};

// Semi-transparent overlay drawn on top of a PanadapterApplet during a
// drag-and-drop operation.  Shows five zones (top/bottom/left/right/center)
// and highlights the active zone with a dim accent block, VSCode-style.
class PanDropOverlay : public QWidget {
    Q_OBJECT

public:
    explicit PanDropOverlay(QWidget* parent = nullptr);

    // Update which zone is highlighted based on the cursor position
    // (in overlay-local coordinates).
    void setActiveZone(DropZone zone);
    DropZone activeZone() const { return m_activeZone; }

    // Determine which zone a local point falls in.
    DropZone zoneAt(const QPoint& localPos) const;

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    DropZone m_activeZone{DropZone::None};
};

} // namespace AetherSDR
