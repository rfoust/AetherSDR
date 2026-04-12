#pragma once

#include <QWidget>

class QTimer;
class QVBoxLayout;

namespace AetherSDR {

class PanadapterApplet;

// Top-level window that hosts a single detached PanadapterApplet.
// The window is frameless with a thin custom title bar. Dragging the
// title bar over the PanadapterStack re-docks the applet. Closing
// the window also re-docks.
class PanFloatingWindow : public QWidget {
    Q_OBJECT

public:
    explicit PanFloatingWindow(PanadapterApplet* applet, QWidget* parent = nullptr);

    PanadapterApplet* applet() const { return m_applet; }
    QString panId() const;

    // Update the displayed title (e.g. "Panadapter — Slice A")
    void setTitle(const QString& title);

    // Take the applet back out of this window (caller becomes new parent).
    // Returns the applet and nulls the internal pointer.
    PanadapterApplet* takeApplet();

    void saveWindowGeometry();
    void restoreWindowGeometry();

    // Mark this window as part of application shutdown (closeEvent will accept).
    void setAppShuttingDown() { m_appShuttingDown = true; }

signals:
    void dockRequested(const QString& panId);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;

private:
    PanadapterApplet* m_applet{nullptr};
    QVBoxLayout*      m_contentLayout{nullptr};
    QTimer*           m_saveTimer{nullptr};
    bool              m_appShuttingDown{false};
};

} // namespace AetherSDR
