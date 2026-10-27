#pragma once

#include <QColor>
#include <QDialog>
#include <QIcon>

class QWidget;

namespace VoicePanel::UiKit {

void applyTheme();
QIcon icon(const QString &name, const QColor &color = QColor("#faf1cc"));
QIcon microphoneIcon(double level, bool recording);

// Wrap a dialog in a frameless, design-system shell: visible border, rounded
// corners, a themed title row with the title and a close button, and drag
// support on the header row. Replaces the native frame which blended into a
// dark desktop and leaked platform fonts/borders.
QWidget *wrapDialog(QDialog &dlg, const QString &title);

} // namespace VoicePanel::UiKit
