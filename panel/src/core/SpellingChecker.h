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

private:
    void *checkerFor(const QString &language);
    void *factory_ = nullptr;            // ISpellCheckerFactory*
    void *checkers_[2] = {nullptr, nullptr};   // ISpellChecker*, ru then en
    bool comInitialized_ = false;
};

} // namespace VoicePanel
