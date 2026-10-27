#pragma once
#include <QDialog>

namespace VoicePanel {
class PanelApp;

class VoicePlaybackDialog : public QDialog {
public:
    explicit VoicePlaybackDialog(PanelApp *app, QWidget *parent = nullptr);
    ~VoicePlaybackDialog() override;
private:
    PanelApp *app_;
};
}
