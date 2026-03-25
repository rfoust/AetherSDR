#pragma once

#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

class AmpApplet : public QWidget {
    Q_OBJECT
public:
    explicit AmpApplet(QWidget* parent = nullptr);

    void setFwdPower(float watts);
    void setSwr(float swr);
    void setTemp(float degC);
    void setModel(const QString& model);

private:
    QLabel*  m_titleLabel{nullptr};
    HGauge*  m_fwdGauge{nullptr};
    HGauge*  m_swrGauge{nullptr};
    HGauge*  m_tempGauge{nullptr};
};

} // namespace AetherSDR
