#include "PanDropOverlay.h"

#include <QPainter>
#include <QPaintEvent>

namespace AetherSDR {

PanDropOverlay::PanDropOverlay(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
}

DropZone PanDropOverlay::zoneAt(const QPoint& localPos) const
{
    const int w = width();
    const int h = height();
    if (w == 0 || h == 0) {
        return DropZone::None;
    }

    // Normalized position [0.0, 1.0]
    const double nx = static_cast<double>(localPos.x()) / w;
    const double ny = static_cast<double>(localPos.y()) / h;

    // Center zone: inner 40% rectangle
    static constexpr double kEdge = 0.30;
    if (nx > kEdge && nx < (1.0 - kEdge) && ny > kEdge && ny < (1.0 - kEdge)) {
        return DropZone::Center;
    }

    // Edge zones: whichever edge is closest
    const double distTop    = ny;
    const double distBottom = 1.0 - ny;
    const double distLeft   = nx;
    const double distRight  = 1.0 - nx;

    const double minDist = std::min({distTop, distBottom, distLeft, distRight});

    if (minDist == distTop) {
        return DropZone::Top;
    }
    if (minDist == distBottom) {
        return DropZone::Bottom;
    }
    if (minDist == distLeft) {
        return DropZone::Left;
    }
    return DropZone::Right;
}

void PanDropOverlay::setActiveZone(DropZone zone)
{
    if (m_activeZone == zone) {
        return;
    }
    m_activeZone = zone;
    update();
}

void PanDropOverlay::paintEvent(QPaintEvent* /*ev*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int w = width();
    const int h = height();

    // Light scrim over the entire widget
    p.fillRect(rect(), QColor(15, 15, 26, 60));  // #0f0f1a at ~24% opacity

    if (m_activeZone == DropZone::None) {
        return;
    }

    // Highlight rect for the active zone
    QRect zoneRect;
    const int halfW = w / 2;
    const int halfH = h / 2;

    switch (m_activeZone) {
    case DropZone::Top:
        zoneRect = QRect(0, 0, w, halfH);
        break;
    case DropZone::Bottom:
        zoneRect = QRect(0, halfH, w, h - halfH);
        break;
    case DropZone::Left:
        zoneRect = QRect(0, 0, halfW, h);
        break;
    case DropZone::Right:
        zoneRect = QRect(halfW, 0, w - halfW, h);
        break;
    case DropZone::Center:
        zoneRect = rect();
        break;
    default:
        return;
    }

    // Accent fill: #00b4d8 at ~30% opacity
    p.fillRect(zoneRect, QColor(0, 180, 216, 77));

    // Border around the zone
    p.setPen(QPen(QColor(0, 180, 216, 140), 2));
    p.drawRect(zoneRect.adjusted(1, 1, -1, -1));
}

} // namespace AetherSDR
