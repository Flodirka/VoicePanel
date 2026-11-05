#pragma once
#include <QList>
#include <QString>
#include <QStringList>

// Windows Spell Checking API (ISpellChecker). It reports misspelled words and
// their suggestions; nothing is replaced here. The language model receives the
// hints and decides whether a suggestion fits the sentence, so technical terms,
// paths and identifiers that the checker flags are never rewritten blindly.
namespace VoicePanel {

struct SpellingHint {
    QString word;
    QStringList suggestions;
    // When the checker itself proposes a fix (CORRECTIVE_ACTION_REPLACE with a
    // replacement string — typical for grammar-style errors like wrong
    // prepositions/agreement), this carries it. Empty for plain misspellings.
    QString proposed;
};

class SpellingChecker {
public:
    SpellingChecker();
    ~SpellingChecker();
    SpellingChecker(const SpellingChecker &) = delete;
    SpellingChecker &operator=(const SpellingChecker &) = delete;

    bool available() const { return factory_ != nullptr; }
    // `language` is "ru", "en" or empty (detected from the text).
    QList<SpellingHint> check(const QString &text, const QString &language, int limit = 8);
    // A known user word is never a misspelling: a term like «дропбокс»
    // would otherwise be offered for "correction" to «дроп бокс» or dropped.
    QList<SpellingHint> filterKnown(QList<SpellingHint> hints, const QStringList &knownWords) const;

private:
    void *checkerFor(const QString &language);
    void *factory_ = nullptr;            // ISpellCheckerFactory*
    void *checkers_[2] = {nullptr, nullptr};   // ISpellChecker*, ru then en
    bool comInitialized_ = false;
};

} // namespace VoicePanel
