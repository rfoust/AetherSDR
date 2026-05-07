#pragma once

#include <QWidget>

class QLabel;

namespace AetherSDR {

class FramelessWindowTitleBar : public QWidget {
    Q_OBJECT

public:
    explicit FramelessWindowTitleBar(const QString& title, QWidget* parent = nullptr);

    void setTitleText(const QString& title);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QLabel* m_titleLabel{nullptr};
};

} // namespace AetherSDR
