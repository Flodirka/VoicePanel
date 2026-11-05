#pragma once

#include <QDialog>
#include <QFormLayout>

namespace VoicePanel {

// The mini settings modal the live windows share: a rounded surface-2 card
// that opens next to its anchor button and closes on Esc, on close, or on any
// click outside (Qt::Popup). Replaces permanently visible settings rows —
// the windows themselves stay minimal, the reference designs keep every knob
// one tap away in exactly such a popover.
class SettingsPopover : public QDialog {
    Q_OBJECT
public:
    SettingsPopover(QWidget *anchor, const QString &title);

    QFormLayout *form() const { return form_; }
    QVBoxLayout *column() const { return column_; }

public slots:
    void openAt();                          // position against the anchor, then show

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QWidget *anchor_ = nullptr;
    QFormLayout *form_ = nullptr;
    QVBoxLayout *column_ = nullptr;
};

} // namespace VoicePanel
