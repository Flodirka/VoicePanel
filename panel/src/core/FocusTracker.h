#pragma once
#include <QString>

// Capture of the focused target (window + editable element) for later insertion.
namespace VoicePanel {

struct FocusTarget {
    quintptr windowHandle = 0;    // HWND
    quintptr elementHandle = 0;   // UIA element (IUIAutomationElement*)
    QString windowTitle;
    QString processName;
    QString elementName;          // accessible name / label, for confirmation UI
    bool isEditable = false;
    bool isValid() const { return windowHandle != 0; }
};

// Snapshot of the currently focused editable element (or focused window when
// the element is not exposed). Call when dictation begins.
FocusTarget captureCurrentFocus();
void releaseFocusTarget(FocusTarget &target);

} // namespace VoicePanel
