#pragma once
#include <QString>
#include <QVector>

namespace VoicePanel {
struct TextChange {
    int originalStart;
    int originalLength;
    int resultStart;
    int resultLength;
};

QVector<TextChange> textChanges(const QString &original, const QString &result);
}
