#include "core/TextDiff.h"
#include <QRegularExpression>

using namespace VoicePanel;

QVector<TextChange> VoicePanel::textChanges(const QString &original, const QString &result) {
    if (original == result) return {};
    struct Token { QString text; int start; };
    const auto tokenize = [](const QString &text) {
        QVector<Token> tokens;
        static const QRegularExpression words(QStringLiteral("\\s+|[\\p{L}\\p{N}_]+|[^\\s\\p{L}\\p{N}_]"));
        auto matches = words.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            tokens.append({match.captured(), int(match.capturedStart())});
        }
        return tokens;
    };
    const auto left = tokenize(original), right = tokenize(result);
    const int rows = int(left.size()), cols = int(right.size());
    // Bound work for long transcripts. A large rewrite remains one reviewable change.
    if (qint64(rows + 1) * (cols + 1) > 250000) {
        int first = 0, tail = 0;
        while (first < original.size() && first < result.size() && original[first] == result[first]) ++first;
        while (tail < original.size() - first && tail < result.size() - first &&
               original[original.size() - tail - 1] == result[result.size() - tail - 1]) ++tail;
        return {{first, int(original.size()) - first - tail, first, int(result.size()) - first - tail}};
    }
    QVector<int> lengths((rows + 1) * (cols + 1));
    const auto at = [&](int i, int j) -> int & { return lengths[i * (cols + 1) + j]; };
    for (int i = rows - 1; i >= 0; --i)
        for (int j = cols - 1; j >= 0; --j)
            at(i, j) = left[i].text == right[j].text ? 1 + at(i + 1, j + 1)
                                                    : qMax(at(i + 1, j), at(i, j + 1));
    QVector<TextChange> changes;
    int i = 0, j = 0;
    bool changing = false;
    while (i < rows || j < cols) {
        if (i < rows && j < cols && left[i].text == right[j].text) {
            changing = false;
            ++i;
            ++j;
            continue;
        }
        if (!changing) {
            changes.append({i < rows ? left[i].start : int(original.size()), 0,
                            j < cols ? right[j].start : int(result.size()), 0});
            changing = true;
        }
        if (i < rows && (j == cols || at(i + 1, j) >= at(i, j + 1))) {
            changes.last().originalLength += int(left[i++].text.size());
        } else changes.last().resultLength += int(right[j++].text.size());
    }
    return changes;
}
