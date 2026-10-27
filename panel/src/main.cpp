#include "app/PanelApp.h"
#include "app/ServiceTray.h"
#include <cstring>

int main(int argc, char *argv[]) {
    if (argc > 1 && std::strcmp(argv[1], "--service-tray") == 0)
        return VoicePanel::runServiceTray(argc, argv);
    VoicePanel::PanelApp app(argc, argv);
    return app.run();
}
