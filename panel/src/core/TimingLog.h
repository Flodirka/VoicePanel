#pragma once
#include <QJsonObject>

// Opt-in latency log for measuring the panel's real speech-to-insertion path.
// Set VOICEPANEL_TIMING_LOG to a file path before starting the panel; each
// event is appended as one JSON line with a monotonic "t_ms". Without the
// variable nothing is written and each call returns after one check.
// tools/bench/panel_timing.py turns the log into latency figures.
namespace VoicePanel::TimingLog {

bool enabled();
qint64 nowMs();                                       // monotonic, same clock as the log
void event(const char *name, const QJsonObject &fields = {});

} // namespace VoicePanel::TimingLog
