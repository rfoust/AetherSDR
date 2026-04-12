#include "PanFloatingWindow.h"
#include "PanadapterApplet.h"
#include "core/AppSettings.h"

#include <QVBoxLayout>
#include <QCloseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

namespace AetherSDR {

PanFloatingWindow::PanFloatingWindow(PanadapterApplet* applet, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_applet(applet)
{
    setWindowTitle(QStringLiteral("Panadapter"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(400, 300);

    setStyleSheet(
        "PanFloatingWindow { background: #0f0f1a; }");

    m_contentLayout = new QVBoxLayout(this);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(0);
    applet->setParent(this);
    applet->setDockButtonVisible(true);
    m_contentLayout->addWidget(applet);
    applet->show();

    // Dock button on applet title bar → re-dock
    connect(applet, &PanadapterApplet::dockRequested,
            this, &PanFloatingWindow::dockRequested);

    resize(800, 500);

    // Debounce geometry saves
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, &PanFloatingWindow::saveWindowGeometry);

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_appShuttingDown = true;
    });
}

QString PanFloatingWindow::panId() const
{
    return m_applet ? m_applet->panId() : QString();
}

PanadapterApplet* PanFloatingWindow::takeApplet()
{
    PanadapterApplet* a = m_applet;
    if (a) {
        a->setDockButtonVisible(false);
        m_contentLayout->removeWidget(a);
        a->setParent(nullptr);
        m_applet = nullptr;
    }
    return a;
}

void PanFloatingWindow::setTitle(const QString& title)
{
    setWindowTitle(title);
}

void PanFloatingWindow::saveWindowGeometry()
{
    auto& s = AppSettings::instance();
    const QString key = QStringLiteral("PanFloat_%1_Geometry").arg(panId());
    const QRect r = geometry();
    s.setValue(key, QStringLiteral("%1,%2,%3,%4")
        .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));
    s.save();
}

void PanFloatingWindow::restoreWindowGeometry()
{
    auto& s = AppSettings::instance();
    const QString key = QStringLiteral("PanFloat_%1_Geometry").arg(panId());
    const QString val = s.value(key, "").toString();
    if (val.isEmpty()) {
        return;
    }
    const QStringList parts = val.split(',');
    if (parts.size() != 4) {
        return;
    }
    const QRect r(parts[0].toInt(), parts[1].toInt(),
                  parts[2].toInt(), parts[3].toInt());
    // Validate the geometry is on a visible screen
    bool onScreen = false;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(r)) {
            onScreen = true;
            break;
        }
    }
    if (onScreen) {
        setGeometry(r);
    }
}

void PanFloatingWindow::closeEvent(QCloseEvent* ev)
{
    if (!m_appShuttingDown) {
        saveWindowGeometry();
        emit dockRequested(panId());
        ev->ignore();  // dockPanadapter will destroy us
    } else {
        ev->accept();
    }
}

void PanFloatingWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    m_saveTimer->start();
}

void PanFloatingWindow::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    m_saveTimer->start();
}

} // namespace AetherSDR
