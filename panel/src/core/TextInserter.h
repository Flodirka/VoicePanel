#pragma once
#include "core/FocusTracker.h"
#include <QString>

// Controlled text insertion into a previously captured focus target.
// Never sends Enter; insertion happens only on explicit request.
namespace VoicePanel {

enum class InsertResult {
    Ok,
    TargetGone,        // window closed or focus element invalidated
    NotEditable,       // element no longer accepts text
    UiaFailed,         // UIA insert attempt failed -> caller may fall back
    ClipboardFallback, // used clipboard+Ctrl+V (direct insertion unavailable)
    ClipboardFailed,
};

struct InsertReport {
    InsertResult result;
    QString detail;    // human-readable context for the panel UI
};

// Insert into the remembered target. Tries UIA Value/Text patterns first
// (no clipboard disturbance), then clipboard+paste as a safe fallback when
// direct insertion is unsupported.
InsertReport insertIntoTarget(const FocusTarget &target, const QString &text);

// Spacing-aware text merge (Pithagoras insertAtCaret semantics):
// never glues words together, never pads at box start/end.
QString mergeAtCaret(const QString &before, const QString &after, const QString &insert);

} // namespace VoicePanel
