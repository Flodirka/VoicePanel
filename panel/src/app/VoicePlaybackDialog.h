#pragma once
#include <QDialog>

class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QLabel;

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
