#include "AmpApplet.h"
#include "HGauge.h"

#include <QVBoxLayout>
#include <QLabel>

namespace AetherSDR {

AmpApplet::AmpApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // Title bar (matches TGXL / S-Meter / other applet title bars)
    auto* titleBar = new QWidget;
    titleBar->setFixedHeight(16);
    titleBar->setStyleSheet(
        "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 #3a4a5a, stop:0.5 #2a3a4a, stop:1 #1a2a38); "
        "border-bottom: 1px solid #0a1a28; }");
    m_titleLabel = new QLabel("POWER GENIUS XL", titleBar);
    m_titleLabel->setStyleSheet("QLabel { background: transparent; color: #8aa8c0; "
                                "font-size: 10px; font-weight: bold; }");
    m_titleLabel->setGeometry(6, 1, 200, 14);
    vbox->addWidget(titleBar);

    // Fwd Power gauge: 0-2000W
    m_fwdGauge = new HGauge(0.0f, 2000.0f, 1500.0f, "Fwd Pwr", "",
        {{0, "0"}, {500, "500"}, {1000, "1000"}, {1500, "1.5k"}, {2000, "2k"}});
    vbox->addWidget(m_fwdGauge);

    // SWR gauge: 1-3
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "SWR", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.0f, "2"}, {2.5f, "2.5"}, {3.0f, "3"}});
    vbox->addWidget(m_swrGauge);

    // Temp gauge: 30-100°C
    m_tempGauge = new HGauge(30.0f, 100.0f, 80.0f, "Temp", "°C",
        {{30, "30"}, {50, "50"}, {80, "80"}, {100, "100"}});
    vbox->addWidget(m_tempGauge);
}

void AmpApplet::setFwdPower(float watts)
{
    m_fwdGauge->setValue(watts);
}

void AmpApplet::setSwr(float swr)
{
    m_swrGauge->setValue(swr);
}

void AmpApplet::setTemp(float degC)
{
    m_tempGauge->setValue(degC);
}

void AmpApplet::setModel(const QString& model)
{
    m_titleLabel->setText(model);
}

} // namespace AetherSDR
