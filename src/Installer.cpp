// Native ordinary-user package manager. No startup writes and no silent mode.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <string>
#include "InstallerCore.h"

namespace {
constexpr int kInstall = 101, kRemove = 102, kInspect = 103, kClose = 104;
HWND g_report = nullptr, g_install = nullptr, g_remove = nullptr, g_inspect = nullptr, g_close = nullptr;
HFONT g_font = nullptr;
std::wstring g_text;
HRESULT g_com = E_FAIL;
bool g_busy = false;
std::wstring Scope() {
    return L"ZBRUSH MODEL THUMBNAILS " + std::wstring(r8package::kVersion) + L"\r\n\r\n"
        L"Show embedded model previews on .ZTL and .ZPR files in Windows Explorer.\r\n"
        L"Install for your Windows account. Administrator access is not needed.\r\n"
        L"You can close this manager after installation.\r\n\r\n"
        L"Remove thumbnails restores the previous Explorer registration.\r\n"
        L"Program files and recovery records stay available for reinstalling.\r\n"
        L"Your model files and their open commands are preserved.\r\n\r\n";
}
void ShowResult(const wchar_t* action, const r8reg::Result& result) {
    g_text = Scope() + action + (result.ok ? L" - completed\r\n" : L" - not completed / refused\r\n") + result.text + L"\r\n";
    SetWindowTextW(g_report, g_text.c_str());
    SendMessageW(g_report, EM_SETSEL, static_cast<WPARAM>(g_text.size()), static_cast<LPARAM>(g_text.size()));
    SendMessageW(g_report, EM_SCROLLCARET, 0, 0);
}
void Buttons(bool enabled) {
    EnableWindow(g_install, enabled); EnableWindow(g_remove, enabled); EnableWindow(g_inspect, enabled); EnableWindow(g_close, enabled);
}
void Action(int id) {
    if (g_busy) return;
    g_busy = true; Buttons(false);
    try {
        if (FAILED(g_com)) ShowResult(L"COM INITIALIZATION", {false, L"STA unavailable. No changes performed."});
        else if (id == kInstall) ShowResult(L"EXPLICIT INSTALL", r8reg::Install());
        else if (id == kRemove) ShowResult(L"EXPLICIT REMOVE EXPLORER REGISTRATION", r8reg::Remove());
        else ShowResult(L"READ-ONLY INSPECTION", r8reg::Inspect());
    } catch (...) {
        SetWindowTextW(g_report, L"An operation or result-display error occurred. Completion is not established.\r\n"
            L"Use Inspect. Do not delete journals, replace payloads or assume registration was restored.");
    }
    Buttons(true); g_busy = false;
}
void Layout(HWND window) {
    RECT area{}; GetClientRect(window, &area);
    const int width = area.right, height = area.bottom;
    if (g_report) MoveWindow(g_report, 12, 12, (std::max)(100, width - 24), (std::max)(100, height - 66), TRUE);
    const int top = (std::max)(12, height - 42);
    if (g_install) MoveWindow(g_install, 12, top, 185, 30, TRUE);
    if (g_remove) MoveWindow(g_remove, 209, top, 245, 30, TRUE);
    if (g_inspect) MoveWindow(g_inspect, 466, top, 180, 30, TRUE);
    if (g_close) MoveWindow(g_close, (std::max)(658, width - 112), top, 100, 30, TRUE);
}
HWND Button(HWND parent, HINSTANCE instance, int id, const wchar_t* label) {
    return CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        const HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        g_report = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 10, 10, window, nullptr, instance, nullptr);
        g_install = Button(window, instance, kInstall, L"Install thumbnails");
        g_remove = Button(window, instance, kRemove, L"Remove thumbnails");
        g_inspect = Button(window, instance, kInspect, L"Check status");
        g_close = Button(window, instance, kClose, L"Close");
        if (!g_report || !g_install || !g_remove || !g_inspect || !g_close) return -1;
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (g_font) SendMessageW(g_report, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(g_report, EM_SETLIMITTEXT, 262144, 0); SetWindowTextW(g_report, g_text.c_str());
        Layout(window); return 0;
    }
    case WM_SIZE: Layout(window); return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            const int id = LOWORD(wParam);
            if (id == kClose && !g_busy) { DestroyWindow(window); return 0; }
            if (id == kInstall || id == kRemove || id == kInspect) { Action(id); return 0; }
        }
        break;
    case WM_CLOSE: if (!g_busy) DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_NCDESTROY: if (g_font) { DeleteObject(g_font); g_font = nullptr; } break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    if (commandLine && *commandLine) return 2;
    g_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComGuard { ~ComGuard() { if (SUCCEEDED(g_com)) CoUninitialize(); } } comGuard;
    try { g_text = Scope() + L"READ-ONLY STARTUP INSPECTION\r\n" +
        (SUCCEEDED(g_com) ? r8reg::Inspect().text : L"COM STA initialization failed. No changes requested."); }
    catch (...) { g_text = L"Read-only startup inspection failed. No changes requested."; }
    constexpr wchar_t className[] = L"ZBrushModelThumbnailInstaller";
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass); klass.lpfnWndProc = Procedure; klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); klass.lpszClassName = className;
    if (!RegisterClassExW(&klass)) return 1;
    const std::wstring title = L"ZBrush Model Thumbnails " + std::wstring(r8package::kVersion) + L" - PID " + std::to_wstring(GetCurrentProcessId());
    HWND window = CreateWindowExW(0, className, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 850, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{}; BOOL status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0)
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    return status == -1 ? 1 : static_cast<int>(message.wParam);
}
