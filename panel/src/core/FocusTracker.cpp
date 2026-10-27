#include "core/FocusTracker.h"

#include <QFileInfo>
#include <windows.h>
#include <comdef.h>
#include <uiautomationclient.h>
#include <psapi.h>

using namespace VoicePanel;

// UI Automation: identify the focused element; editability = Value or Text
// pattern availability (both cover editors, browsers, Electron controls).
static bool patternAvailable(IUIAutomationElement *el, PROPERTYID prop) {
    VARIANT v;
    VariantInit(&v);
    bool avail = false;
    if (SUCCEEDED(el->GetCurrentPropertyValue(prop, &v))) {
        avail = v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE;
        VariantClear(&v);
    }
    return avail;
}

namespace VoicePanel {

FocusTarget retainFocusTarget(const FocusTarget &target) {
    if (target.elementHandle)
        reinterpret_cast<IUIAutomationElement *>(target.elementHandle)->AddRef();
    return target;
}

void releaseFocusTarget(FocusTarget &target) {
    if (target.elementHandle)
        reinterpret_cast<IUIAutomationElement *>(target.elementHandle)->Release();
    target = {};
}

FocusTarget captureCurrentFocus() {
    FocusTarget target;
    const HWND hwnd = GetForegroundWindow();
    if (!hwnd) return target;
    target.windowHandle = reinterpret_cast<quintptr>(hwnd);

    const int len = GetWindowTextLengthW(hwnd);
    if (len > 0) {
        wchar_t *buf = new wchar_t[len + 1];
        GetWindowTextW(hwnd, buf, len + 1);
        target.windowTitle = QString::fromWCharArray(buf, len);
        delete[] buf;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t name[MAX_PATH] = {};
            if (GetProcessImageFileNameW(h, name, MAX_PATH)) {
                target.processName = QFileInfo(QString::fromWCharArray(name)).fileName();
            }
            CloseHandle(h);
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(hr);
    IUIAutomation *automation = nullptr;
    IUIAutomationElement *focused = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IUIAutomation), (void **)&automation)) &&
        SUCCEEDED(automation->GetFocusedElement(&focused)) && focused) {
        target.isEditable = patternAvailable(focused, UIA_IsValuePatternAvailablePropertyId) ||
                            patternAvailable(focused, UIA_IsTextPatternAvailablePropertyId);
        BSTR name = nullptr;
        if (SUCCEEDED(focused->get_CurrentName(&name)) && name) {
            target.elementName = QString::fromWCharArray(name);
            SysFreeString(name);
        }
        // MainWindow releases this reference when it replaces the target.
        focused->AddRef();
        target.elementHandle = reinterpret_cast<quintptr>(focused);
        focused->Release();  // drop the reference owned by GetFocusedElement
    }
    if (automation) automation->Release();
    if (uninit) CoUninitialize();
    return target;
}

} // namespace VoicePanel
