#include "core/SpellingChecker.h"

#include <string>

#include <windows.h>
#include <spellcheck.h>

using namespace VoicePanel;

namespace {

bool hasCyrillic(const QString &text) {
    for (const QChar ch : text) {
        if (ch.unicode() >= 0x0400 && ch.unicode() <= 0x04FF) return true;
    }
    return false;
}

// Only plain words are worth reporting: identifiers, paths, versions and
// acronyms are the model's business, not the spell checker's.
bool worthReporting(const QString &word) {
    if (word.size() < 2 || word.size() > 48) return false;
    bool letter = false;
    for (int i = 0; i < word.size(); ++i) {
        const QChar ch = word.at(i);
        if (ch.isLetter()) {
            if (i > 0 && ch.isUpper()) return false;   // CamelCase, acronyms
            letter = true;
        } else if (ch != QLatin1Char('\'') && ch != QChar(0x2019) && ch != QLatin1Char('-')) {
            return false;                              // digits, dots, slashes, underscores
        }
    }
    return letter;
}

QStringList suggestionsFor(ISpellChecker *checker, const QString &word, int limit) {
    QStringList out;
    IEnumString *enumeration = nullptr;
    if (FAILED(checker->Suggest(reinterpret_cast<LPCWSTR>(const_cast<ushort *>(word.utf16())), &enumeration)) ||
        !enumeration)
        return out;
    LPOLESTR text = nullptr;
    while (out.size() < limit && enumeration->Next(1, &text, nullptr) == S_OK) {
        if (text) {
            out.append(QString::fromWCharArray(text));
            CoTaskMemFree(text);
        }
    }
    enumeration->Release();
    return out;
}

} // namespace

SpellingChecker::SpellingChecker() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized_ = SUCCEEDED(initialized) && initialized != S_FALSE;
    // RPC_E_CHANGED_MODE (already initialised as MTA) is fine: the checker is
    // usable from either apartment.
    ISpellCheckerFactory *factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(ISpellCheckerFactory),
                                   reinterpret_cast<void **>(&factory)))) {
        factory_ = factory;
    }
}

SpellingChecker::~SpellingChecker() {
    for (void *&checker : checkers_) {
        if (checker) {
            static_cast<ISpellChecker *>(checker)->Release();
            checker = nullptr;
        }
    }
    if (factory_) {
        static_cast<ISpellCheckerFactory *>(factory_)->Release();
        factory_ = nullptr;
    }
    if (comInitialized_) CoUninitialize();
}

void *SpellingChecker::checkerFor(const QString &language) {
    const int slot = language == "ru" ? 0 : 1;
    if (checkers_[slot]) return checkers_[slot];
    if (!factory_) return nullptr;
    auto *factory = static_cast<ISpellCheckerFactory *>(factory_);
    const wchar_t *tag = slot == 0 ? L"ru-RU" : L"en-US";
    ISpellChecker *checker = nullptr;
    if (FAILED(factory->CreateSpellChecker(tag, &checker))) return nullptr;
    checkers_[slot] = checker;
    return checker;
}

QList<SpellingHint> SpellingChecker::check(const QString &text, const QString &language, int limit) {
    QList<SpellingHint> hints;
    if (!factory_ || text.trimmed().isEmpty() || limit <= 0) return hints;
    const QString chosen = language == "ru" || language == "en"
                               ? language
                               : (hasCyrillic(text) ? QString("ru") : QString("en"));
    auto *checker = static_cast<ISpellChecker *>(checkerFor(chosen));
    if (!checker) return hints;

    const std::wstring wide = text.toStdWString();
    IEnumSpellingError *errors = nullptr;
    if (FAILED(checker->Check(wide.c_str(), &errors)) || !errors) return hints;
    ISpellingError *error = nullptr;
    while (hints.size() < limit && errors->Next(&error) == S_OK) {
        ULONG start = 0, length = 0;
        error->get_StartIndex(&start);
        error->get_Length(&length);
        error->Release();
        if (length == 0 || start + length > wide.size()) continue;
        const QString word = QString::fromWCharArray(wide.c_str() + start, int(length));
        if (!worthReporting(word)) continue;
        hints.append({word, suggestionsFor(checker, word, 3)});
    }
    errors->Release();
    return hints;
}
