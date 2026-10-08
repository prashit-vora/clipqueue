/* Development-only desktop test: a real Win32 EDIT source/destination and a separate daemon. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
static HWND daemon, frame, source, sink;
static PROCESS_INFORMATION process;
static void pump(unsigned ms) {
    ULONGLONG end = GetTickCount64() + ms;
    MSG msg;
    do {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    } while (GetTickCount64() < end);
}
static void fail(const char *why) {
    fprintf(stderr, "FAIL: %s (Windows error %lu)\n", why, GetLastError());
    if (daemon)
        SendMessageW(daemon, WM_APP + 1, 6, 0);
    if (process.hProcess) {
        WaitForSingleObject(process.hProcess, 2000);
        CloseHandle(process.hProcess);
    }
    exit(1);
}
static void check(int ok, const char *why) {
    if (!ok)
        fail(why);
}
/* Public native control protocol, shared by the command-line client. */
static size_t ctl(unsigned cmd) {
    DWORD_PTR reply = 0;
    check(SendMessageTimeoutW(daemon, WM_APP + 1, cmd, 0, SMTO_ABORTIFHUNG, 2000, &reply) && reply,
          "control reply");
    return (size_t)((reply - 1) / 2);
}
static void focus(HWND edit) {
    ShowWindow(frame, SW_SHOW);
    SetForegroundWindow(frame);
    SetFocus(edit);
    pump(100);
    check(GetForegroundWindow() == frame && GetFocus() == edit, "focus native test editor");
}
static void keys(WORD key, unsigned count, int alt) {
    INPUT input[64];
    ZeroMemory(input, sizeof(input));
    unsigned n = 0;
    input[n++].ki.wVk = VK_CONTROL;
    if (alt)
        input[n++].ki.wVk = VK_MENU;
    for (unsigned i = 0; i < count; i++) {
        input[n++].ki.wVk = key;
        input[n].ki.wVk = key;
        input[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    if (alt) {
        input[n].ki.wVk = VK_MENU;
        input[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    input[n].ki.wVk = VK_CONTROL;
    input[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    for (unsigned i = 0; i < n; i++)
        input[i].type = INPUT_KEYBOARD;
    check(SendInput(n, input, sizeof(INPUT)) == n, "send native shortcut");
}
static void copy(const wchar_t *text) {
    focus(source);
    SetWindowTextW(source, text);
    SendMessageW(source, EM_SETSEL, 0, -1);
    keys('C', 1, 0);
    pump(220);
}
static void expect(const wchar_t *wanted, const char *label) {
    wchar_t actual[4096];
    GetWindowTextW(sink, actual, 4096);
    if (wcscmp(wanted, actual)) {
        fwprintf(stderr, L"Expected [%ls], got [%ls]\n", wanted, actual);
        fail(label);
    }
    printf("PASS: %s\n", label);
}
int wmain(int argc, wchar_t **argv) {
    check(argc == 2, "daemon executable argument");
    wchar_t command[32768];
    swprintf(command, 32768, L"\"%ls\" --daemon", argv[1]);
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    check(CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si,
                         &process),
          "start daemon");
    CloseHandle(process.hThread);
    for (int i = 0; i < 50 && !daemon; i++) {
        pump(100);
        daemon = FindWindowExW(HWND_MESSAGE, NULL, L"ClipQueue.Native.1", NULL);
    }
    check(daemon != NULL, "daemon ready");
    frame = CreateWindowExW(0, L"STATIC", L"ClipQueue native integration test",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 600, 300, NULL, NULL,
                            GetModuleHandleW(NULL), NULL);
    source = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE, 10,
                             10, 560, 90, frame, NULL, NULL, NULL);
    sink = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE, 10,
                           110, 560, 90, frame, NULL, NULL, NULL);
    check(frame && source && sink, "create native text controls");
    focus(sink);
    keys('Q', 1, 1);
    pump(200); /* real toggle shortcut */
    copy(L"A");
    copy(L"A");
    copy(L"B");
    copy(L"C");
    check(ctl(1) == 3, "copy keyboard unchanged and consecutive dedup");
    focus(sink);
    keys('V', 3, 0);
    pump(1200);
    expect(L"ABC", "three rapid Ctrl+V presses paste three FIFO entries");
    check(ctl(1) == 0, "no own-write recapture");
    check(ctl(7) == 1, "undo restores last dispatched entry");
    keys('V', 1, 0);
    pump(350);
    expect(L"ABCC", "restored entry pastes again");
    keys('V', 1, 0);
    pump(200);
    expect(L"ABCCC", "empty queue passes through");
    keys('Q', 1, 1);
    pump(150);
    copy(L"paused");
    check(ctl(1) == 0, "paused copies are ignored");
    focus(sink);
    keys('V', 1, 0);
    pump(200);
    expect(L"ABCCCpaused", "paused paste passes through");
    keys('Q', 1, 1);
    pump(150);
    copy(L"clear me");
    check(ctl(1) == 1, "queue resumed");
    focus(sink);
    keys(VK_BACK, 1, 1);
    pump(200);
    check(ctl(1) == 0, "clear keyboard shortcut");
    copy(L"snowman \x2603");
    focus(sink);
    keys('V', 1, 0);
    pump(350);
    expect(L"ABCCCpausedsnowman \x2603", "Unicode keyboard paste");
    ctl(6);
    check(WaitForSingleObject(process.hProcess, 3000) == WAIT_OBJECT_0, "graceful exit");
    CloseHandle(process.hProcess);
    process.hProcess = NULL;
    DestroyWindow(frame);
    puts("PASS: Windows real-window keyboard integration");
    return 0;
}
