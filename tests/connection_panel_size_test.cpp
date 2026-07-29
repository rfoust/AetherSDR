// Regression harness for issue #4515: the Connect to Radio window must keep
// its Disconnect footer reachable when its body is taller than the screen.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/ConnectionPanel.h"

#include <QApplication>
#include <QFont>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

int bottomInPanel(QWidget* widget, QWidget* panel)
{
    return widget->mapTo(panel, QPoint(0, widget->height())).y();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-connection-panel-size-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("ConnectionPanel screen-fit test harness (#4515)\n\n");

    const QFont originalFont = app.font();
    for (const qreal scale : {1.0, 1.25, 1.5}) {
        QFont scaledFont = originalFont;
        scaledFont.setPointSizeF(originalFont.pointSizeF() * scale);
        app.setFont(scaledFont);

        ConnectionPanel panel;
        panel.setMinimumSize(640, 360);
        panel.resize(760, 360);
        panel.setConnected(true);
        panel.show();
        QApplication::processEvents();

        QScrollArea* body =
            panel.findChild<QScrollArea*>(QStringLiteral("connectionBodyScrollArea"));
        QPushButton* disconnect =
            panel.findChild<QPushButton*>(QStringLiteral("connectionDisconnectButton"));

        const std::string suffix = " scale=" + std::to_string(scale);
        report("scrollable connection body exists",
               body != nullptr,
               suffix);
        report("body overflows into a vertical scrollbar",
               body && body->verticalScrollBar()->maximum() > 0,
               suffix + " maximum="
                   + std::to_string(body ? body->verticalScrollBar()->maximum() : -1));
        report("Disconnect remains visible",
               disconnect && disconnect->isVisible(),
               suffix);
        report("Disconnect remains inside the panel",
               disconnect && bottomInPanel(disconnect, &panel) <= panel.height(),
               suffix + " bottom="
                   + std::to_string(disconnect ? bottomInPanel(disconnect, &panel) : -1)
                   + " panelH=" + std::to_string(panel.height()));
        report("Disconnect footer stays below the scrolling body",
               body && disconnect
                   && disconnect->mapTo(&panel, QPoint()).y()
                       >= body->mapTo(&panel, QPoint(0, body->height())).y(),
               suffix);

        panel.hide();
    }
    app.setFont(originalFont);

    ConnectionPanel oversizedPanel;
    oversizedPanel.setMinimumSize(640, 360);
    oversizedPanel.resize(760, 10000);
    QScreen* screen = QApplication::primaryScreen();
    oversizedPanel.fitToScreen(screen);
    const int availableHeight = screen ? screen->availableGeometry().height() : 0;
    report("screen-fit clamps an oversized panel to available height",
           availableHeight > 0 && oversizedPanel.frameGeometry().height() <= availableHeight,
           "frameH=" + std::to_string(oversizedPanel.frameGeometry().height())
               + " availableH=" + std::to_string(availableHeight));

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
