#include "core/TextInserter.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QRegularExpression>
#include <QTimer>
#include <windows.h>
#include <comdef.h>
#include <uiautomationclient.h>
#include <memory>
#include <wrl/client.h>

using namespace VoicePanel;

namespace VoicePanel {

InsertReport insertIntoTarget(const FocusTarget &target, const QString &text) {
    InsertReport report{InsertResult::UiaFailed, {}};
    if (!target.isValid()) {
        report.result = InsertResult::TargetGone;
        return report;
    }
    const HWND hwnd = reinterpret_cast<HWND>(target.windowHandle);
    if (!IsWindow(hwnd)) {
        report.result = InsertResult::TargetGone;
        report.detail = QObject::tr("target window was closed");
        return report;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(hr);
    QString insertion = text;

    // 1) Direct insertion through UIA (does not touch the clipboard).
    if (target.elementHandle) {
        auto *el = reinterpret_cast<IUIAutomationElement *>(target.elementHandle);
        BOOL enabled = FALSE;
        if (SUCCEEDED(el->get_CurrentIsEnabled(&enabled)) && enabled) {
            IUIAutomationValuePattern *value = nullptr;
            IUIAutomationTextPattern *textProv = nullptr;
            bool handled = false;
            if (!handled && SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, (IUnknown **)&value)) && value) {
                BSTR cur = nullptr;
                const HRESULT readHr = value->get_CurrentValue(&cur);
                const bool empty = SUCCEEDED(readHr) && (!cur || SysStringLen(cur) == 0);
                if (cur) SysFreeString(cur);
                if (empty) {
                    BSTR bstr = SysAllocStringLen(reinterpret_cast<const wchar_t *>(text.constData()),
                                                 UINT(text.size()));
                    handled = SUCCEEDED(value->SetValue(bstr));
                    SysFreeString(bstr);
                    report.detail = QObject::tr("UIA value pattern");
                }
                value->Release();
            }
            if (!handled && SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId, (IUnknown **)&textProv)) && textProv) {
                // Read only the characters beside the selection. All insertion
                // still uses the existing clipboard fallback below.
                using Microsoft::WRL::ComPtr;
                ComPtr<IUIAutomationTextRangeArray> selections;
                ComPtr<IUIAutomationTextRange> selection, before, after;
                if (SUCCEEDED(textProv->GetSelection(&selections)) && selections &&
                    SUCCEEDED(selections->GetElement(0, &selection)) && selection &&
                    SUCCEEDED(selection->Clone(&before)) && SUCCEEDED(selection->Clone(&after))) {
                    int moved = 0;
                    BSTR left = nullptr, right = nullptr;
                    if (SUCCEEDED(before->MoveEndpointByRange(TextPatternRangeEndpoint_End, selection.Get(), TextPatternRangeEndpoint_Start)) &&
                        SUCCEEDED(before->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved)) &&
                        SUCCEEDED(before->GetText(1, &left)) &&
                        SUCCEEDED(after->MoveEndpointByRange(TextPatternRangeEndpoint_Start, selection.Get(), TextPatternRangeEndpoint_End)) &&
                        SUCCEEDED(after->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1, &moved)) &&
                        SUCCEEDED(after->GetText(1, &right))) {
                        const QString prefix = QString::fromWCharArray(left ? left : L"");
                        const QString suffix = QString::fromWCharArray(right ? right : L"");
                        const QString merged = mergeAtCaret(prefix, suffix, text);
                        insertion = merged.mid(prefix.size(), merged.size() - prefix.size() - suffix.size());
                    }
                    if (left) SysFreeString(left);
                    if (right) SysFreeString(right);
                }
                textProv->Release();
            }
            if (handled) {
                report.result = InsertResult::Ok;
                if (uninit) CoUninitialize();
                return report;
            }
        }
    }

    // 2) Safe fallback: focus the window and paste via clipboard.
    // The old clipboard content is restored afterwards on a best-effort basis.
    QClipboard *clipboard = QApplication::clipboard();
    auto saved = std::make_shared<QMimeData>();
    if (const auto *mime = clipboard->mimeData())
        for (const QString &format : mime->formats()) saved->setData(format, mime->data(format));
    clipboard->setText(insertion);
    if (clipboard->text() != insertion) {
        report.result = InsertResult::ClipboardFailed;
        report.detail = QObject::tr("could not write to the clipboard");
        if (uninit) CoUninitialize();
        return report;
    }
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    const auto restoreClipboard = [clipboard, clipboardSequence, saved]() {
        if (GetClipboardSequenceNumber() != clipboardSequence) return;
        auto *restored = new QMimeData;
        for (const QString &format : saved->formats()) restored->setData(format, saved->data(format));
        clipboard->setMimeData(restored);
    };
    if (GetForegroundWindow() == hwnd || SetForegroundWindow(hwnd)) {
        if (target.elementHandle && target.isEditable) {
            auto *element = reinterpret_cast<IUIAutomationElement *>(target.elementHandle);
            if (FAILED(element->SetFocus())) {
                restoreClipboard();
                report.result = InsertResult::ClipboardFailed;
                report.detail = QObject::tr("could not focus the remembered text field");
                if (uninit) CoUninitialize();
                return report;
            }
        }
        Sleep(60);  // let the foreground switch settle
        INPUT ctrlV[4] = {};
        ctrlV[0].type = INPUT_KEYBOARD;
        ctrlV[0].ki.wVk = VK_CONTROL;
        ctrlV[1].type = INPUT_KEYBOARD;
        ctrlV[1].ki.wVk = 'V';
        ctrlV[2].type = INPUT_KEYBOARD;
        ctrlV[2].ki.wVk = 'V';
        ctrlV[2].ki.dwFlags = KEYEVENTF_KEYUP;
        ctrlV[3].type = INPUT_KEYBOARD;
        ctrlV[3].ki.wVk = VK_CONTROL;
        ctrlV[3].ki.dwFlags = KEYEVENTF_KEYUP;
        const UINT sent = SendInput(4, ctrlV, sizeof(INPUT));
        QTimer::singleShot(1000, clipboard, restoreClipboard);
        report.result = sent == 4 ? InsertResult::ClipboardFallback : InsertResult::ClipboardFailed;
        report.detail = QObject::tr("clipboard paste");
    } else {
        restoreClipboard();
        report.result = InsertResult::ClipboardFailed;
        report.detail = QObject::tr("could not focus target window");
    }
    if (uninit) CoUninitialize();
    return report;
}

static struct ElementReleaser {
    ~ElementReleaser() { /* process exit: elements die with COM */ }
} g_elementReleaser;

QString mergeAtCaret(const QString &before, const QString &after, const QString &insert) {
    static const QRegularExpression trailingSpace("\\s$");
    static const QRegularExpression leadingSpace("^\\s");
    const QString lead = !before.isEmpty() && !trailingSpace.match(before).hasMatch() && !leadingSpace.match(insert).hasMatch() ? " " : "";
    const QString trail = !after.isEmpty() && !leadingSpace.match(after).hasMatch() && !trailingSpace.match(insert).hasMatch() ? " " : "";
    return before + lead + insert + trail + after;
}

} // namespace VoicePanel
