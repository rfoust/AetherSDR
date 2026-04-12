#include "PanadapterApplet.h"
#include "GuardedSlider.h"
#include "SpectrumWidget.h"
#include "core/AppSettings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QApplication>
#include <QPainter>
#include <QTimer>

namespace AetherSDR {

PanadapterApplet::PanadapterApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Title bar (16px gradient, matching applet style) ─────────────────
    m_titleBarWidget = new QWidget;
    m_titleBarWidget->setFixedHeight(16);
    m_titleBarWidget->setCursor(Qt::OpenHandCursor);
    m_titleBarWidget->installEventFilter(this);
    m_titleBarWidget->setStyleSheet(
        "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 #3a4a5a, stop:0.5 #2a3a4a, stop:1 #1a2a38); "
        "border-bottom: 1px solid #0a1a28; }");

    auto* barLayout = new QHBoxLayout(m_titleBarWidget);
    barLayout->setContentsMargins(6, 1, 4, 1);
    barLayout->setSpacing(2);

    m_titleLabel = new QLabel;
    m_titleLabel->setStyleSheet("QLabel { background: transparent; color: #8aa8c0; "
                                "font-size: 10px; font-weight: bold; }");
    barLayout->addWidget(m_titleLabel);
    barLayout->addStretch();

    const QString btnStyle = QStringLiteral(
        "QPushButton { background: transparent; color: #6a8090; "
        "border: none; font-size: 9px; padding: 0; }"
        "QPushButton:hover { color: #c8d8e8; }");

    // ── Dock button (hidden by default, shown when floating) ─────────
    m_dockBtn = new QPushButton(QStringLiteral("\u21a9 Dock"));
    m_dockBtn->setFixedHeight(14);
    m_dockBtn->setCursor(Qt::ArrowCursor);
    m_dockBtn->setToolTip("Return panadapter to the main window");
    m_dockBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #8aa8c0; "
        "border: 1px solid #304050; border-radius: 3px; "
        "font-size: 9px; padding: 0 5px; }"
        "QPushButton:hover { background: #243848; color: #c8d8e8; }");
    m_dockBtn->hide();
    connect(m_dockBtn, &QPushButton::clicked, this, [this]() {
        emit dockRequested(m_panId);
    });
    barLayout->addWidget(m_dockBtn);

    auto* closeBtn = new QPushButton("\u00D7");
    closeBtn->setFixedSize(14, 14);
    closeBtn->setCursor(Qt::ArrowCursor);
    closeBtn->setStyleSheet(btnStyle + "QPushButton:hover { color: #ff4040; }");
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        emit closeRequested(m_panId);
    });
    barLayout->addWidget(closeBtn);

    layout->addWidget(m_titleBarWidget);

    // ── Spectrum widget (FFT + waterfall) ────────────────────────────────
    m_spectrum = new SpectrumWidget(this);
    m_spectrum->installEventFilter(this);  // detect clicks for pan activation
    layout->addWidget(m_spectrum, 1);

    // ── CW decode panel (hidden by default, shown in CW mode) ─────────
    m_cwPanel = new QWidget(this);
    m_cwPanel->setCursor(Qt::ArrowCursor);  // prevent invisible cursor from native-window parent (#1096)
    m_cwPanel->setFixedHeight(80);
    m_cwPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_cwPanel->setStyleSheet("QWidget { background: #0a0a14; border-top: 1px solid #203040; }");

    auto* cwLayout = new QVBoxLayout(m_cwPanel);
    cwLayout->setContentsMargins(4, 2, 4, 2);
    cwLayout->setSpacing(1);

    // Stats bar: pitch, speed, clear button
    auto* cwBar = new QHBoxLayout;
    cwBar->setSpacing(6);
    auto* cwTitle = new QLabel("CW");
    cwTitle->setStyleSheet("QLabel { color: #00b4d8; font-size: 10px; font-weight: bold; background: transparent; }");
    cwBar->addWidget(cwTitle);
    auto* cwHint = new QLabel("(requires PC Audio)");
    cwHint->setStyleSheet("QLabel { color: #405060; font-size: 9px; background: transparent; }");
    cwBar->addWidget(cwHint);

    m_cwStatsLabel = new QLabel;
    m_cwStatsLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 10px; background: transparent; }");
    cwBar->addWidget(m_cwStatsLabel);

    // Sensitivity slider — filters low-confidence decodes
    auto* sensLabel = new QLabel("Sens:");
    sensLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 9px; background: transparent; }");
    cwBar->addWidget(sensLabel);
    m_cwSensSlider = new GuardedSlider(Qt::Horizontal);
    m_cwSensSlider->setRange(0, 100);  // 0=show everything, 100=only high confidence
    int savedSens = AppSettings::instance().value("CwDecoderSensitivity", "30").toString().toInt();
    m_cwSensSlider->setValue(savedSens);
    m_cwSensSlider->setFixedWidth(60);
    m_cwSensSlider->setStyleSheet(
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #00b4d8; width: 10px; margin: -3px 0; border-radius: 5px; }");
    m_cwCostThreshold = 1.0f - (savedSens / 100.0f) * 0.9f;
    connect(m_cwSensSlider, &QSlider::valueChanged, this, [this](int v) {
        // Map 0-100 slider to 1.0-0.1 cost threshold (inverted: higher sens = lower threshold)
        m_cwCostThreshold = 1.0f - (v / 100.0f) * 0.9f;
        AppSettings::instance().setValue("CwDecoderSensitivity", QString::number(v));
        AppSettings::instance().save();
    });
    cwBar->addWidget(m_cwSensSlider);

    // Lock Pitch button
    m_lockPitchBtn = new QPushButton("\xF0\x9F\x94\x92P");  // 🔒P
    m_lockPitchBtn->setCheckable(true);
    m_lockPitchBtn->setFixedSize(28, 16);
    m_lockPitchBtn->setToolTip("Lock decoder pitch to current frequency");
    m_lockPitchBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #6a8090; border: 1px solid #203040;"
        " border-radius: 2px; font-size: 8px; padding: 0; }"
        "QPushButton:checked { color: #00b4d8; border-color: #00b4d8; }"
        "QPushButton:hover { color: #c8d8e8; }");
    cwBar->addWidget(m_lockPitchBtn);

    // Lock Speed button
    m_lockSpeedBtn = new QPushButton("\xF0\x9F\x94\x92S");  // 🔒S
    m_lockSpeedBtn->setCheckable(true);
    m_lockSpeedBtn->setFixedSize(28, 16);
    m_lockSpeedBtn->setToolTip("Lock decoder speed to current WPM");
    m_lockSpeedBtn->setStyleSheet(m_lockPitchBtn->styleSheet());
    cwBar->addWidget(m_lockSpeedBtn);

    // Pitch range sliders — constrain decoder frequency search
    const QString rangeSliderStyle =
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #6a8090; width: 8px; margin: -3px 0; border-radius: 4px; }";

    auto* minLabel = new QLabel("Lo:");
    minLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    cwBar->addWidget(minLabel);

    m_pitchMinSlider = new GuardedSlider(Qt::Horizontal);
    m_pitchMinSlider->setRange(300, 1200);
    m_pitchMinSlider->setValue(500);
    m_pitchMinSlider->setFixedWidth(50);
    m_pitchMinSlider->setStyleSheet(rangeSliderStyle);
    m_pitchMinSlider->setToolTip("Decoder pitch search minimum (Hz)");
    cwBar->addWidget(m_pitchMinSlider);
    m_pitchMinValLabel = new QLabel(QString::number(m_pitchMinSlider->value()));
    m_pitchMinValLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    m_pitchMinValLabel->setFixedWidth(24);
    cwBar->addWidget(m_pitchMinValLabel);

    auto* maxLabel = new QLabel("Hi:");
    maxLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    cwBar->addWidget(maxLabel);

    m_pitchMaxSlider = new GuardedSlider(Qt::Horizontal);
    m_pitchMaxSlider->setRange(300, 1200);
    m_pitchMaxSlider->setValue(700);
    m_pitchMaxSlider->setFixedWidth(50);
    m_pitchMaxSlider->setStyleSheet(rangeSliderStyle);
    m_pitchMaxSlider->setToolTip("Decoder pitch search maximum (Hz)");
    cwBar->addWidget(m_pitchMaxSlider);
    m_pitchMaxValLabel = new QLabel(QString::number(m_pitchMaxSlider->value()));
    m_pitchMaxValLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    m_pitchMaxValLabel->setFixedWidth(24);
    cwBar->addWidget(m_pitchMaxValLabel);

    // Update tooltips and emit range change — clamp so min ≤ max
    connect(m_pitchMinSlider, &QSlider::valueChanged, this, [this](int v) {
        if (v > m_pitchMaxSlider->value()) {
            QSignalBlocker b(m_pitchMinSlider);
            m_pitchMinSlider->setValue(m_pitchMaxSlider->value());
            v = m_pitchMaxSlider->value();
        }
        m_pitchMinSlider->setToolTip(QString("%1 Hz").arg(v));
        m_pitchMinValLabel->setText(QString::number(v));
        emit pitchRangeChanged(v, m_pitchMaxSlider->value());
    });
    connect(m_pitchMaxSlider, &QSlider::valueChanged, this, [this](int v) {
        if (v < m_pitchMinSlider->value()) {
            QSignalBlocker b(m_pitchMaxSlider);
            m_pitchMaxSlider->setValue(m_pitchMinSlider->value());
            v = m_pitchMinSlider->value();
        }
        m_pitchMaxSlider->setToolTip(QString("%1 Hz").arg(v));
        m_pitchMaxValLabel->setText(QString::number(v));
        emit pitchRangeChanged(m_pitchMinSlider->value(), v);
    });
    m_pitchMinSlider->setToolTip(QString("%1 Hz").arg(m_pitchMinSlider->value()));
    m_pitchMaxSlider->setToolTip(QString("%1 Hz").arg(m_pitchMaxSlider->value()));

    cwBar->addStretch();

    auto* clearBtn = new QPushButton("CLR");
    clearBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #8090a0; border: 1px solid #203040;"
        " border-radius: 2px; font-size: 9px; font-weight: bold;"
        " padding: 1px 6px; }"
        "QPushButton:hover { color: #c8d8e8; background: #2a3a4a; }");
    connect(clearBtn, &QPushButton::clicked, this, &PanadapterApplet::clearCwText);
    cwBar->addWidget(clearBtn);

    cwLayout->addLayout(cwBar);

    // Text area
    m_cwText = new QTextEdit;
    m_cwText->setReadOnly(true);
    m_cwText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_cwText->setStyleSheet(
        "QTextEdit { background: #0a0a14; color: #00ff88; border: none;"
        " font-family: monospace; font-size: 13px; font-weight: bold; }"
        "QScrollBar:vertical { width: 6px; background: #0a0a14; }"
        "QScrollBar::handle:vertical { background: #304050; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    m_cwText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_cwText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cwText->setWordWrapMode(QTextOption::WrapAnywhere);
    cwLayout->addWidget(m_cwText);

    m_cwPanel->hide();
    layout->addWidget(m_cwPanel);
}

void PanadapterApplet::setSliceId(int id)
{
    Q_UNUSED(id);
    // Title bar is intentionally blank — slice info shown in the spectrum overlay
}

void PanadapterApplet::clearSliceTitle()
{
    m_titleLabel->clear();
}

void PanadapterApplet::setDockButtonVisible(bool visible)
{
    m_dockBtn->setVisible(visible);
}

void PanadapterApplet::setCwPanelVisible(bool visible)
{
    m_cwPanel->setVisible(visible);
}

void PanadapterApplet::appendCwText(const QString& text, float cost)
{
    // Filter by sensitivity threshold — drop low-confidence decodes
    if (cost >= m_cwCostThreshold) return;

    // Strip newlines — ggmorse inserts them on pitch changes, but we want
    // continuous flowing text. Replace with space to preserve word boundaries.
    QString clean = text;
    clean.replace('\n', ' ');

    // Color by confidence: lower cost = higher confidence
    //   < 0.15  green   (high confidence)
    //   < 0.35  yellow  (medium)
    //   < 0.60  orange  (meh)
    //   >= 0.60 red     (low confidence)
    QString color;
    if (cost < 0.15f)      color = "#00ff88";
    else if (cost < 0.35f) color = "#e0e040";
    else if (cost < 0.60f) color = "#ff9020";
    else                   color = "#ff4040";

    m_cwText->moveCursor(QTextCursor::End);
    m_cwText->insertHtml(QString("<span style=\"color:%1\">%2</span>")
        .arg(color, clean.toHtmlEscaped()));
    m_cwText->moveCursor(QTextCursor::End);
}

void PanadapterApplet::setCwStats(float pitchHz, float speedWpm)
{
    if (pitchHz > 0 && speedWpm > 0)
        m_cwStatsLabel->setText(QString("%1 Hz  %2 WPM").arg(pitchHz, 0, 'f', 0).arg(speedWpm, 0, 'f', 0));
}

void PanadapterApplet::clearCwText()
{
    m_cwText->clear();
}

bool PanadapterApplet::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::MouseButtonPress) {
        emit activated(m_panId);

        // Title bar drag initiation: record start position
        if (obj == m_titleBarWidget) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                m_dragStartPos = me->pos();
            }
        }
    }

    // Title bar drag threshold check
    if (obj == m_titleBarWidget && ev->type() == QEvent::MouseMove) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if ((me->buttons() & Qt::LeftButton) && !m_dragStartPos.isNull()) {
            if ((me->pos() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
                m_titleBarWidget->setCursor(Qt::ClosedHandCursor);

                auto* drag = new QDrag(this);
                auto* mimeData = new QMimeData;
                mimeData->setData(kMimeType, m_panId.toUtf8());
                drag->setMimeData(mimeData);

                // Build a semi-transparent preview thumbnail
                QPixmap snapshot = this->grab();
                constexpr int kPreviewWidth = 320;
                QSize previewSize = snapshot.size().scaled(
                    kPreviewWidth, kPreviewWidth, Qt::KeepAspectRatio);
                snapshot = snapshot.scaled(
                    previewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QPixmap previewPm(snapshot.size());
                previewPm.fill(Qt::transparent);
                {
                    QPainter painter(&previewPm);
                    painter.setOpacity(0.65);
                    painter.drawPixmap(0, 0, snapshot);
                    painter.setOpacity(1.0);
                    painter.setPen(QPen(QColor(0x00, 0xb4, 0xd8, 180), 2));
                    painter.drawRect(previewPm.rect().adjusted(1, 1, -1, -1));
                }

                // Show the preview as a floating window that tracks the cursor.
                // The QDrag pixmap is 1x1 transparent so the OS snap-back
                // animation is invisible when the drop is not accepted.
                QPixmap transparent(1, 1);
                transparent.fill(Qt::transparent);
                drag->setPixmap(transparent);

                auto* previewWin = new QWidget(nullptr,
                    Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
                previewWin->setAttribute(Qt::WA_TranslucentBackground);
                previewWin->setAttribute(Qt::WA_ShowWithoutActivating);
                previewWin->setFixedSize(previewPm.size() / previewPm.devicePixelRatio());
                auto* pmLabel = new QLabel(previewWin);
                pmLabel->setPixmap(previewPm);
                pmLabel->setGeometry(0, 0, previewWin->width(), previewWin->height());
                previewWin->move(QCursor::pos() - QPoint(previewWin->width() / 2, 12));
                previewWin->show();

                auto* tracker = new QTimer(previewWin);
                connect(tracker, &QTimer::timeout, previewWin, [previewWin]() {
                    previewWin->move(QCursor::pos() - QPoint(previewWin->width() / 2, 12));
                });
                tracker->start(16);  // ~60 fps

                emit dragStarted(m_panId);
                m_dragStartPos = QPoint();  // reset

                Qt::DropAction result = drag->exec(Qt::MoveAction);

                // Destroy the floating preview immediately
                delete previewWin;

                // If the drop was not accepted by any target (released outside
                // the PanadapterStack), signal that we should float this pan.
                if (result == Qt::IgnoreAction) {
                    emit dragDroppedOutside(m_panId);
                }

                m_titleBarWidget->setCursor(Qt::OpenHandCursor);
                return true;
            }
        }
    }

    // Reset drag on release
    if (obj == m_titleBarWidget && ev->type() == QEvent::MouseButtonRelease) {
        m_dragStartPos = QPoint();
    }

    return QWidget::eventFilter(obj, ev);
}

} // namespace AetherSDR
