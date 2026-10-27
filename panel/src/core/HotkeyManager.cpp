#include "core/HotkeyManager.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QKeySequence>
#include <windows.h>

namespace VoicePanel {

static quint32 parseModifiers(const QString &combo, QString *keyOut) {
    quint32 mods = 0;
    QString rest = combo;
    const QStringList parts = combo.split('+');
    keyOut->clear();
    for (int i = 0; i < parts.size(); ++i) {
        const QString p = parts[i].trimmed().toLower();
        if (i == parts.size() - 1) {
            *keyOut = parts[i].trimmed();
        } else if (p == "ctrl" || p == "control") {
            mods |= MOD_CONTROL;
        } else if (p == "alt") {
            mods |= MOD_ALT;
        } else if (p == "shift") {
            mods |= MOD_SHIFT;
        } else if (p == "win" || p == "meta") {
            mods |= MOD_WIN;
        }
    }
    return mods;
}

class HotkeyNativeFilter : public QAbstractNativeEventFilter {
public:
    explicit HotkeyNativeFilter(HotkeyManager *owner) : owner_(owner) {}
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override {
        if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") return false;
        const auto *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_HOTKEY) {
            owner_->fire(int(msg->wParam));
            return true;
        }
        return false;
    }

private:
    HotkeyManager *owner_;
};

HotkeyManager::HotkeyManager(QObject *parent) : QObject(parent) {
    filter_ = new HotkeyNativeFilter(this);
    qApp->installNativeEventFilter(filter_);
}

HotkeyManager::~HotkeyManager() {
    unregisterAll();
    qApp->removeNativeEventFilter(filter_);
    delete filter_;
}

bool HotkeyManager::validCombo(const QString &combo) {
    QString key;
    const auto modifiers = parseModifiers(combo, &key);
    if (key.isEmpty() || !modifiers) return false;
    const QKeySequence seq(key);
    return !seq.isEmpty() && int(seq[0]) != 0 && int(seq[0]) != int(Qt::Key_unknown);
}

bool HotkeyManager::registerHotkey(const QString &combo) {
    QString key;
    const quint32 mods = parseModifiers(combo, &key);
    const QKeySequence seq(key);
    if (seq.isEmpty()) return false;
    const UINT vk = UINT(seq[0]);
    if (!mods) return false;  // require at least one modifier: avoid eating plain keys
    const int id = nextId_++;
    if (!RegisterHotKey(nullptr, id, mods | MOD_NOREPEAT, vk)) {
        return false;
    }
    active_.insert(id, combo);
    return true;
}

void HotkeyManager::unregisterAll() {
    for (auto it = active_.cbegin(); it != active_.cend(); ++it) {
        UnregisterHotKey(nullptr, it.key());
    }
    active_.clear();
}

void HotkeyManager::fire(int id) {
    if (active_.contains(id)) emit triggered(active_.value(id));
}

} // namespace VoicePanel
