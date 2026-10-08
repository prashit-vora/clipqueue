/* Native Windows backend. Only Windows system DLLs are required. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "core.h"
#include <objbase.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>
#include <windows.h>

#define CLASS_NAME L"ClipQueue.Native.1"
#define WM_CONTROL (WM_APP + 1)
#define WM_QUEUE_PASTE (WM_APP + 2)
#define OWN_INPUT ((ULONG_PTR)0x43515545)
static CqQueue queue;
static HWND window;
static CqRequests requests;
static HHOOK hook;
static UINT png_format;
static DWORD seen_sequence, own_sequence;
static int v_held, paste_attempts, capture_attempts;
static ULONGLONG last_paste;
static IWICImagingFactory *wic;
static int com_initialized;

enum { CMD_STATUS = 1, CMD_ON, CMD_OFF, CMD_TOGGLE, CMD_CLEAR, CMD_STOP, CMD_UNDO };
static void notice(int ok) {
    MessageBeep(ok ? MB_OK : MB_ICONEXCLAMATION);
}

static void image_identity(CqItem *p) {
    if (!wic) {
        if (!com_initialized) {
            if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
                return;
            com_initialized = 1;
        }
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                    &IID_IWICImagingFactory, (void **)&wic)))
            return;
    }
    size_t prefix = (p->format == CF_DIB || p->format == CF_DIBV5) ? sizeof(BITMAPFILEHEADER) : 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, p->size + prefix);
    if (!mem)
        return;
    unsigned char *out = GlobalLock(mem);
    if (!out) {
        GlobalFree(mem);
        return;
    }
    if (prefix) {
        if (p->size < sizeof(BITMAPINFOHEADER)) {
            GlobalUnlock(mem);
            GlobalFree(mem);
            return;
        }
        BITMAPINFOHEADER h;
        memcpy(&h, p->data, sizeof(h));
        if (h.biSize < sizeof(h) || h.biSize > p->size || h.biBitCount > 32) {
            GlobalUnlock(mem);
            GlobalFree(mem);
            return;
        }
        size_t colors =
            h.biClrUsed ? h.biClrUsed : (h.biBitCount <= 8 ? (size_t)1 << h.biBitCount : 0);
        size_t offset = prefix + h.biSize + colors * sizeof(RGBQUAD);
        if (h.biSize == sizeof(h) && h.biCompression == BI_BITFIELDS)
            offset += 12;
        if (offset > prefix + p->size) {
            GlobalUnlock(mem);
            GlobalFree(mem);
            return;
        }
        BITMAPFILEHEADER file = {0};
        file.bfType = 0x4d42;
        file.bfSize = (DWORD)(prefix + p->size);
        file.bfOffBits = (DWORD)offset;
        memcpy(out, &file, prefix);
    }
    memcpy(out + prefix, p->data, p->size);
    GlobalUnlock(mem);
    IStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    BYTE *pixels = NULL;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return;
    }
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(
            wic, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder)))
        goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)))
        goto done;
    UINT width = 0, height = 0;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)) || !width || !height ||
        (uint64_t)width * height > 16u * 1024u * 1024u)
        goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(wic, &converter)))
        goto done;
    if (FAILED(IWICFormatConverter_Initialize(
            converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)))
        goto done;
    UINT bytes = width * height * 4;
    pixels = malloc(bytes);
    if (pixels &&
        SUCCEEDED(IWICFormatConverter_CopyPixels(converter, NULL, width * 4, bytes, pixels)))
        cq_pixel_hash(p, pixels, width, height);
done:
    free(pixels);
    if (converter)
        IWICFormatConverter_Release(converter);
    if (frame)
        IWICBitmapFrameDecode_Release(frame);
    if (decoder)
        IWICBitmapDecoder_Release(decoder);
    IStream_Release(stream);
}

/* Copy while the clipboard is open; expensive image decoding happens after closing. */
static CqItem *read_clipboard(void) {
    CqItem *p = NULL;
    UINT formats[] = {png_format, CF_DIBV5, CF_DIB, CF_UNICODETEXT};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(*formats) && !p; ++i) {
        UINT format = formats[i];
        if (!IsClipboardFormatAvailable(format))
            continue;
        HGLOBAL handle = GetClipboardData(format);
        SIZE_T bytes = handle ? GlobalSize(handle) : 0;
        if (!bytes || bytes > CQ_MAX_ITEM_BYTES)
            continue;
        const void *data = GlobalLock(handle);
        if (!data)
            continue;
        if (format == CF_UNICODETEXT) {
            const wchar_t *s = data;
            size_t n = 0;
            while (n < bytes / sizeof(wchar_t) && s[n])
                n++;
            if (n && n < bytes / sizeof(wchar_t)) {
                int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, (int)n, NULL, 0,
                                              NULL, NULL);
                char *utf8 = len > 0 ? malloc((size_t)len) : NULL;
                if (utf8) {
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, (int)n, utf8, len, NULL,
                                        NULL);
                    p = cq_item(utf8, (size_t)len, format, 0);
                    free(utf8);
                }
            }
        } else
            p = cq_item(data, bytes, format, 1);
        GlobalUnlock(handle);
    }
    return p;
}
static void capture(void) {
    DWORD sequence = GetClipboardSequenceNumber();
    if (!queue.enabled || sequence == seen_sequence || sequence == own_sequence)
        return;
    if (!OpenClipboard(window)) {
        if (++capture_attempts < 20)
            SetTimer(window, 1, 25, NULL);
        return;
    }
    CqItem *p = read_clipboard();
    seen_sequence = GetClipboardSequenceNumber();
    CloseClipboard();
    capture_attempts = 0;
    if (p) {
        if (p->image)
            image_identity(p);
        if (cq_push(&queue, p) < 0)
            notice(0);
    }
}
static int publish(CqItem *p) {
    int wide = p->image ? 0
                        : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)p->data,
                                              (int)p->size, NULL, 0);
    if (!p->image && wide <= 0)
        return 0;
    SIZE_T bytes = p->image ? p->size : ((SIZE_T)wide + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle)
        return 0;
    void *data = GlobalLock(handle);
    if (!data) {
        GlobalFree(handle);
        return 0;
    }
    if (p->image)
        memcpy(data, p->data, bytes);
    else {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)p->data, (int)p->size, data,
                            wide);
        ((wchar_t *)data)[wide] = 0;
    }
    GlobalUnlock(handle);
    if (!OpenClipboard(window)) {
        GlobalFree(handle);
        return 0;
    }
    int ok = EmptyClipboard() && SetClipboardData(p->format, handle) != NULL;
    own_sequence = seen_sequence = GetClipboardSequenceNumber();
    CloseClipboard();
    if (!ok)
        GlobalFree(handle);
    return ok;
}
#include "../tests/portable/native_clipboard_test.h"

static int send_paste(void) {
    INPUT input[4];
    memset(input, 0, sizeof(input));
    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    unsigned n = 0;
    if (!ctrl) {
        input[n].ki.wVk = VK_CONTROL;
        n++;
    }
    input[n].ki.wVk = 'V';
    n++;
    input[n].ki.wVk = 'V';
    input[n].ki.dwFlags = KEYEVENTF_KEYUP;
    n++;
    if (!ctrl) {
        input[n].ki.wVk = VK_CONTROL;
        input[n].ki.dwFlags = KEYEVENTF_KEYUP;
        n++;
    }
    for (unsigned i = 0; i < n; i++) {
        input[i].type = INPUT_KEYBOARD;
        input[i].ki.dwExtraInfo = OWN_INPUT;
    }
    return SendInput(n, input, sizeof(INPUT)) == n;
}
static void cancel_pastes(void) {
    cq_cancel_requests(&requests);
    paste_attempts = 0;
    KillTimer(window, 2);
}
static void paste(void) {
    CqPasteRequest *request = cq_next_request(&requests);
    if (!request)
        return;
    if ((uintptr_t)GetForegroundWindow() != request->target) {
        cancel_pastes();
        notice(0);
        return;
    }
    if (GetTickCount64() - last_paste < 150) {
        SetTimer(window, 2, 10, NULL);
        return;
    }
    /* A modifier pressed after the original V must not turn the replay into a different shortcut.
     */
    if ((GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) |
         GetAsyncKeyState(VK_RWIN)) &
        0x8000) {
        SetTimer(window, 2, 10, NULL);
        return;
    }
    capture();
    CqItem *p = queue.enabled ? cq_peek(&queue) : NULL;
    if (p && !publish(p)) {
        if (++paste_attempts < 20) {
            SetTimer(window, 2, 25, NULL);
            return;
        }
        cancel_pastes();
        notice(0);
        return;
    }
    if (!send_paste()) {
        cancel_pastes();
        notice(0);
        return;
    }
    if (p)
        cq_commit(&queue);
    last_paste = GetTickCount64();
    paste_attempts = 0;
    cq_finish_request(&requests);
    if (requests.count)
        SetTimer(window, 2, 10, NULL);
}
static LRESULT CALLBACK keyboard(int code, WPARAM msg, LPARAM value) {
    KBDLLHOOKSTRUCT *key = (KBDLLHOOKSTRUCT *)value;
    if (code == HC_ACTION && key->dwExtraInfo != OWN_INPUT && key->vkCode == 'V') {
        if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && v_held) {
            v_held = 0;
            return 1;
        }
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (v_held)
                return 1;
            if (queue.enabled && (queue.count || requests.count) &&
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
                !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_LWIN) & 0x8000) &&
                !(GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                v_held = 1;
                if (cq_request(&requests, (uintptr_t)GetForegroundWindow(), 'V'))
                    PostMessageW(window, WM_QUEUE_PASTE, 0, 0);
                else
                    notice(0);
                return 1;
            }
        }
    }
    return CallNextHookEx(hook, code, msg, value);
}
static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CLIPBOARDUPDATE:
        capture_attempts = 0;
        capture();
        return 0;
    case WM_QUEUE_PASTE:
        paste();
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, w);
        if (w == 1)
            capture();
        else if (w == 2)
            paste();
        else if (w == 3)
            PostQuitMessage(0);
        return 0;
    case WM_HOTKEY:
        w = w == 1 ? CMD_TOGGLE : CMD_CLEAR; /* fall through */
    case WM_CONTROL:
        switch (w) {
        case CMD_ON:
            cq_enable(&queue, 1);
            seen_sequence = GetClipboardSequenceNumber();
            break;
        case CMD_OFF:
            cancel_pastes();
            cq_enable(&queue, 0);
            break;
        case CMD_TOGGLE:
            cancel_pastes();
            cq_enable(&queue, !queue.enabled);
            seen_sequence = GetClipboardSequenceNumber();
            break;
        case CMD_CLEAR:
            cancel_pastes();
            cq_clear(&queue);
            break;
        case CMD_UNDO:
            cancel_pastes();
            if (cq_undo(&queue) < 0)
                return -1;
            break;
        case CMD_STOP:
            PostQuitMessage(0);
            break;
        }
        if (msg == WM_HOTKEY)
            notice(queue.enabled);
        return (LRESULT)(1 + queue.count * 2 + queue.enabled);
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
static void memory_report(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    memset(&m, 0, sizeof(m));
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m)))
        printf("{\"working_set_bytes\":%llu,\"private_bytes\":%llu,\"keyboard_hook\":%s}\n",
               (unsigned long long)m.WorkingSetSize, (unsigned long long)m.PrivateUsage,
               hook ? "true" : "false");
}
static int daemon_main(int probe, int selftest) {
    if (FindWindowExW(HWND_MESSAGE, NULL, CLASS_NAME, NULL))
        return 0;
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = procedure;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = CLASS_NAME;
    if (!RegisterClassW(&cls))
        return 1;
    window = CreateWindowExW(0, CLASS_NAME, L"ClipQueue", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                             cls.hInstance, NULL);
    if (!window)
        return 1;
    png_format = RegisterClipboardFormatW(L"PNG");
    seen_sequence = GetClipboardSequenceNumber();
    if (selftest) {
        int result = native_clipboard_test(CF_UNICODETEXT, png_format);
        DestroyWindow(window);
        if (wic)
            IWICImagingFactory_Release(wic);
        if (com_initialized)
            CoUninitialize();
        return result;
    }
    if (!AddClipboardFormatListener(window))
        return 1;
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, cls.hInstance, 0);
    int keys = RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q') &&
               RegisterHotKey(window, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_BACK);
    if (!hook || !keys) {
        fprintf(stderr,
                "ClipQueue: could not register keyboard controls (shortcut already in use?).\n");
        return 1;
    }
    if (probe)
        SetTimer(window, 3, 2000, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (probe)
        memory_report();
    UnhookWindowsHookEx(hook);
    UnregisterHotKey(window, 1);
    UnregisterHotKey(window, 2);
    RemoveClipboardFormatListener(window);
    DestroyWindow(window);
    cq_clear(&queue);
    if (wic)
        IWICImagingFactory_Release(wic);
    if (com_initialized)
        CoUninitialize();
    return 0;
}
static int run(const wchar_t *arg) {
    if (!wcscmp(arg, L"--daemon"))
        return daemon_main(0, 0);
    if (!wcscmp(arg, L"--probe"))
        return daemon_main(1, 0);
    if (!wcscmp(arg, L"--self-test"))
        return daemon_main(0, 1);
    HWND running = FindWindowExW(HWND_MESSAGE, NULL, CLASS_NAME, NULL);
    if (!*arg || !wcscmp(arg, L"start")) {
        if (running)
            return 0;
        wchar_t path[32768], command[32784];
        if (!GetModuleFileNameW(NULL, path, 32768))
            return 1;
        swprintf(command, 32784, L"\"%ls\" --daemon", path);
        STARTUPINFOW startup = {0};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process;
        if (!CreateProcessW(path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                            NULL, NULL, &startup, &process))
            return 1;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    }
    unsigned cmd = !wcscmp(arg, L"status")   ? CMD_STATUS
                   : !wcscmp(arg, L"on")     ? CMD_ON
                   : !wcscmp(arg, L"off")    ? CMD_OFF
                   : !wcscmp(arg, L"toggle") ? CMD_TOGGLE
                   : !wcscmp(arg, L"clear")  ? CMD_CLEAR
                   : !wcscmp(arg, L"undo")   ? CMD_UNDO
                   : !wcscmp(arg, L"stop")   ? CMD_STOP
                                             : 0;
    if (!cmd) {
        fprintf(stderr, "Usage: clipqueue [start|status|on|off|toggle|clear|undo|stop]\n");
        return 2;
    }
    if (!running) {
        puts("ClipQueue is not running");
        return cmd == CMD_STOP ? 0 : 1;
    }
    DWORD_PTR reply = 0;
    if (!SendMessageTimeoutW(running, WM_CONTROL, cmd, 0, SMTO_ABORTIFHUNG, 2000, &reply) || !reply)
        return 1;
    if (reply == (DWORD_PTR)-1) {
        fputs("Cannot restore: queue is full.\n", stderr);
        return 1;
    }
    printf("ClipQueue: %s, %llu queued\n", (reply - 1) & 1 ? "on" : "off",
           (unsigned long long)((reply - 1) / 2));
    return 0;
}
#ifdef CQ_CONSOLE
int wmain(int argc, wchar_t **argv) {
    return run(argc > 1 ? argv[1] : L"start");
}
#else
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, wchar_t *args, int show) {
    (void)instance;
    (void)previous;
    (void)show;
    /* Keep redirected handles for scripts; attach to an existing console for CLI use. */
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    return run(args);
}
#endif
