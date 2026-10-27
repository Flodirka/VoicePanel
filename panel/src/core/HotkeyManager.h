#pragma once
#include <QObject>
#include <QHash>

// Global hotkeys via RegisterHotKey, delivered through Qt's native event filter.
namespace VoicePanel {

class HotkeyManager : public QObject {
    Q_OBJECT
public:
    explicit HotkeyManager(QObject *parent = nullptr);
    ~HotkeyManager() override;

    static bool validCombo(const QString &combo);   // e.g. "Ctrl+Alt+D"

    bool registerHotkey(const QString &combo);      // emits triggered(combo)
    void unregisterAll();
    int count() const { return active_.size(); }

signals:
    void triggered(const QString &combo);

private:
    friend class HotkeyNativeFilter;
    void fire(int id);

    QHash<int, QString> active_;
    int nextId_ = 1;
    class HotkeyNativeFilter *filter_ = nullptr;
};

} // namespace VoicePanel
