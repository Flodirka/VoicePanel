#include "app/PanelApp.h"
#include "app/ServiceTray.h"
#include <cstring>

int main(int argc, char *argv[]) {
    if (argc > 1 && std::strcmp(argv[1], "--service-tray") == 0)
        return VoicePanel::runServiceTray(argc, argv);
    VoicePanel::PanelApp app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--check-bundle") == 0)
        return app.runCheckBundle();
    if (argc > 2 && std::strcmp(argv[1], "--capture-ui") == 0)
        return app.runCaptureUi(QString::fromLocal8Bit(argv[2]));
    return app.run();
}
