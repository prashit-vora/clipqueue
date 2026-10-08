/* Native macOS backend: system frameworks only, no AppKit application/window. */
#include "core.h"
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#define OWN_INPUT 0x43515545
static CqQueue queue;
static PasteboardRef board;
static CFMachPortRef tap;
static CFRunLoopSourceRef paste_source;
static int control_fd = -1, lock_fd = -1, pending, held[128];
static pid_t paste_pid;
static CGKeyCode paste_key;
static CFAbsoluteTime last_paste;
static volatile sig_atomic_t stopping;
static char directory[80], socket_path[104];
static CFStringRef flavor(unsigned n) {
    switch (n) {
    case 1:
        return CFSTR("public.png");
    case 2:
        return CFSTR("public.tiff");
    case 3:
        return CFSTR("public.jpeg");
    case 4:
        return CFSTR("public.utf16-plain-text");
    default:
        return CFSTR("public.utf8-plain-text");
    }
}
static void notice(void) { /* State is also available through `clipqueue status`. */
    fputs("\a", stderr);
}
static void image_identity(CqItem *item) {
    CFDataRef data =
        CFDataCreateWithBytesNoCopy(NULL, item->data, (CFIndex)item->size, kCFAllocatorNull);
    if (!data)
        return;
    CGImageSourceRef source = CGImageSourceCreateWithData(data, NULL);
    CFRelease(data);
    if (!source)
        return;
    /* Read dimensions before decoding to bound memory use. */
    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(source, 0, NULL);
    int w = 0, h = 0;
    if (props) {
        CFNumberRef wn = CFDictionaryGetValue(props, kCGImagePropertyPixelWidth),
                    hn = CFDictionaryGetValue(props, kCGImagePropertyPixelHeight);
        if (wn)
            CFNumberGetValue(wn, kCFNumberIntType, &w);
        if (hn)
            CFNumberGetValue(hn, kCFNumberIntType, &h);
        CFRelease(props);
    }
    if (w <= 0 || h <= 0 || (uint64_t)w * (unsigned)h > 16u * 1024u * 1024u) {
        CFRelease(source);
        return;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!image)
        return;
    size_t bytes = (size_t)w * (size_t)h * 4;
    unsigned char *pixels = calloc(1, bytes);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        pixels && color
            ? CGBitmapContextCreate(pixels, (size_t)w, (size_t)h, 8, (size_t)w * 4, color,
                                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
            : NULL;
    if (context) {
        CGContextSetBlendMode(context, kCGBlendModeCopy);
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
        for (size_t n = 0; n < bytes; n += 4) {
            unsigned a = pixels[n + 3];
            for (int c = 0; c < 3; c++)
                pixels[n + c] =
                    a ? (unsigned char)(((unsigned)pixels[n + c] * 255 + a / 2) / a) : 0;
        }
        cq_pixel_hash(item, pixels, (uint32_t)w, (uint32_t)h);
        CGContextRelease(context);
    }
    if (color)
        CGColorSpaceRelease(color);
    free(pixels);
    CGImageRelease(image);
}
static CqItem *read_clipboard(void) {
    ItemCount count = 0;
    if (PasteboardGetItemCount(board, &count) != noErr || !count)
        return NULL;
    PasteboardItemID id;
    if (PasteboardGetItemIdentifier(board, 1, &id) != noErr)
        return NULL;
    const unsigned formats[] = {1, 2, 3, 0, 4};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
        unsigned format = formats[i];
        CFDataRef data = NULL;
        if (PasteboardCopyItemFlavorData(board, id, flavor(format), &data) != noErr || !data)
            continue;
        CFIndex size = CFDataGetLength(data);
        CqItem *p = NULL;
        if (size > 0 && size <= CQ_MAX_ITEM_BYTES) {
            if (format == 4) {
                CFStringRef text =
                    CFStringCreateFromExternalRepresentation(NULL, data, kCFStringEncodingUTF16);
                CFDataRef utf8 =
                    text
                        ? CFStringCreateExternalRepresentation(NULL, text, kCFStringEncodingUTF8, 0)
                        : NULL;
                if (utf8) {
                    p = cq_item(CFDataGetBytePtr(utf8), (size_t)CFDataGetLength(utf8), 0, 0);
                    CFRelease(utf8);
                }
                if (text)
                    CFRelease(text);
            } else
                p = cq_item(CFDataGetBytePtr(data), (size_t)size, format, format != 0);
        }
        CFRelease(data);
        if (p)
            return p;
    }
    return NULL;
}
static void capture(void) {
    PasteboardSyncFlags flags = PasteboardSynchronize(board);
    if (!queue.enabled || !(flags & kPasteboardModified) || (flags & kPasteboardClientIsOwner))
        return;
    CqItem *p = read_clipboard();
    if (p) {
        if (p->image)
            image_identity(p);
        if (cq_push(&queue, p) < 0)
            notice();
    }
}
static int publish(CqItem *p) {
    CFDataRef data = CFDataCreate(NULL, p->data, (CFIndex)p->size);
    if (!data)
        return 0;
    OSStatus status = PasteboardClear(board);
    if (status == noErr)
        status = PasteboardPutItemFlavor(board, (PasteboardItemID)1, flavor(p->format), data, 0);
    CFRelease(data);
    PasteboardSynchronize(board);
    return status == noErr;
}
#include "../tests/portable/native_clipboard_test.h"

static pid_t front_pid(void) {
    ProcessSerialNumber psn;
    pid_t pid = 0;
    if (GetFrontProcess(&psn) == noErr)
        GetProcessPID(&psn, &pid);
    return pid;
}
static void perform_paste(void *unused) {
    (void)unused;
    if (!pending)
        return;
    if (front_pid() != paste_pid) {
        pending = 0;
        notice();
        return;
    }
    if (CFAbsoluteTimeGetCurrent() - last_paste < 0.15)
        return;
    capture();
    CqItem *p = queue.enabled ? cq_peek(&queue) : NULL;
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, paste_key, true),
               up = CGEventCreateKeyboardEvent(NULL, paste_key, false);
    if (!down || !up || !CGPreflightPostEventAccess() || (p && !publish(p))) {
        if (down)
            CFRelease(down);
        if (up)
            CFRelease(up);
        pending = 0;
        notice();
        return;
    }
    CGEventSetFlags(down, kCGEventFlagMaskCommand);
    CGEventSetFlags(up, kCGEventFlagMaskCommand);
    CGEventSetIntegerValueField(down, kCGEventSourceUserData, OWN_INPUT);
    CGEventSetIntegerValueField(up, kCGEventSourceUserData, OWN_INPUT);
    CGEventPost(kCGSessionEventTap, down);
    CGEventPost(kCGSessionEventTap, up);
    CFRelease(down);
    CFRelease(up);
    if (p)
        cq_pop(&queue);
    last_paste = CFAbsoluteTimeGetCurrent();
    pending = 0;
}
static CGEventRef keyboard(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                           void *unused) {
    (void)proxy;
    (void)unused;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(tap, true);
        return event;
    }
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == OWN_INPUT)
        return event;
    unsigned code = (unsigned)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (code >= 128)
        return event;
    if (type == kCGEventKeyUp && held[code]) {
        held[code] = 0;
        return NULL;
    }
    if (type != kCGEventKeyDown)
        return event;
    if (held[code])
        return NULL;
    CGEventFlags flags = CGEventGetFlags(event);
    flags &= kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate | kCGEventFlagMaskControl |
             kCGEventFlagMaskShift;
    UniChar chars[4];
    UniCharCount length = 0;
    CGEventKeyboardGetUnicodeString(event, 4, &length, chars);
    int v = length == 1 && (chars[0] == 'v' || chars[0] == 'V');
    /* Option can change Q's produced character; ANSI keycode 12 is the control fallback. */
    int q = (length == 1 && (chars[0] == 'q' || chars[0] == 'Q')) || code == 12;
    if (flags == (kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate) && (q || code == 51)) {
        held[code] = 1;
        if (q) {
            cq_enable(&queue, !queue.enabled);
            PasteboardSynchronize(board);
        } else
            cq_clear(&queue);
        notice();
        return NULL;
    }
    if (flags == kCGEventFlagMaskCommand && v && queue.enabled && queue.count) {
        held[code] = 1;
        if (!pending) {
            paste_pid = front_pid();
            paste_key = (CGKeyCode)code;
            pending = 1;
            CFRunLoopSourceSignal(paste_source);
        }
        return NULL;
    }
    return event;
}
static int prepare_paths(void) {
    snprintf(directory, sizeof(directory), "/tmp/clipqueue-%lu", (unsigned long)getuid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
        return 0;
    struct stat st;
    if (lstat(directory, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077))
        return 0;
    snprintf(socket_path, sizeof(socket_path), "%s/control", directory);
    return 1;
}
static int open_control(void) {
    char path[104];
    snprintf(path, sizeof(path), "%s/lock", directory);
    lock_fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB))
        return 0;
    control_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (control_fd < 0)
        return 0;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    unlink(socket_path);
    if (bind(control_fd, (struct sockaddr *)&addr, sizeof(addr)))
        return 0;
    fcntl(control_fd, F_SETFL, O_NONBLOCK);
    return 1;
}
static void controls(void) {
    char command[32];
    struct sockaddr_un peer;
    socklen_t size = sizeof(peer);
    ssize_t n =
        recvfrom(control_fd, command, sizeof(command) - 1, 0, (struct sockaddr *)&peer, &size);
    if (n <= 0)
        return;
    command[n] = 0;
    int valid = 1;
    if (!strcmp(command, "on")) {
        cq_enable(&queue, 1);
        PasteboardSynchronize(board);
    } else if (!strcmp(command, "off"))
        cq_enable(&queue, 0);
    else if (!strcmp(command, "toggle")) {
        cq_enable(&queue, !queue.enabled);
        PasteboardSynchronize(board);
    } else if (!strcmp(command, "clear"))
        cq_clear(&queue);
    else if (!strcmp(command, "stop"))
        stopping = 1;
    else if (strcmp(command, "status"))
        valid = 0;
    char reply[160];
    snprintf(reply, sizeof(reply),
             valid ? "ClipQueue: %s, %zu queued, keyboard %s\n" : "Unknown command: %s\n",
             queue.enabled ? "on" : "off", queue.count, tap ? "ready" : "permission required");
    sendto(control_fd, reply, strlen(reply), 0, (struct sockaddr *)&peer, size);
}
static void tick(CFRunLoopTimerRef timer, void *unused) {
    (void)timer;
    (void)unused;
    controls();
    capture();
    if (pending)
        perform_paste(NULL);
    if (stopping)
        CFRunLoopStop(CFRunLoopGetCurrent());
}
static void signal_stop(int sig) {
    (void)sig;
    stopping = 1;
}
static void memory_report(void) {
    mach_task_basic_info_data_t m;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&m, &count) == KERN_SUCCESS)
        printf("{\"resident_bytes\":%llu,\"keyboard_hook\":%s}\n",
               (unsigned long long)m.resident_size, tap ? "true" : "false");
}
static int daemon_main(int probe, int selftest, int ready_fd) {
    if (PasteboardCreate(kPasteboardClipboard, &board) != noErr)
        return 1;
    PasteboardSynchronize(board);
    if (selftest) {
        int result = native_clipboard_test(0, 1);
        CFRelease(board);
        return result;
    }
    if (!open_control()) {
        fprintf(stderr, "ClipQueue: already running or cannot create control socket.\n");
        return 1;
    }
    CFRunLoopSourceContext context = {0};
    context.perform = perform_paste;
    paste_source = CFRunLoopSourceCreate(NULL, 0, &context);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), paste_source, kCFRunLoopCommonModes);
    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                           CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp),
                           keyboard, NULL);
    CFRunLoopSourceRef source = tap ? CFMachPortCreateRunLoopSource(NULL, tap, 0) : NULL;
    if (source)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    if (!tap && !probe) {
        fprintf(stderr, "ClipQueue needs Accessibility permission. Enable ClipQueue in System "
                        "Settings > Privacy & Security > Accessibility, then restart it.\n");
        const void *keys[] = {kAXTrustedCheckOptionPrompt};
        const void *values[] = {kCFBooleanTrue};
        CFDictionaryRef options =
            CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
        unlink(socket_path);
        close(control_fd);
        close(lock_fd);
        return 1;
    }
    CFRunLoopTimerRef timer =
        CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.1, 0.1, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    signal(SIGTERM, signal_stop);
    signal(SIGINT, signal_stop);
    if (ready_fd >= 0) {
        char ready = 1;
        (void)write(ready_fd, &ready, 1);
        close(ready_fd);
    }
    if (probe) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2, false);
        memory_report();
    } else
        CFRunLoopRun();
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    if (source)
        CFRelease(source);
    if (tap)
        CFRelease(tap);
    CFRelease(paste_source);
    CFRelease(board);
    cq_clear(&queue);
    close(control_fd);
    unlink(socket_path);
    close(lock_fd);
    return 0;
}
static int command(const char *cmd, int quiet) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;
    struct sockaddr_un local = {0}, remote = {0};
    local.sun_family = remote.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "%s/client-%lu", directory,
             (unsigned long)getpid());
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", socket_path);
    unlink(local.sun_path);
    int ok = !bind(fd, (struct sockaddr *)&local, sizeof(local)) &&
             sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&remote, sizeof(remote)) >= 0;
    struct pollfd pollfd = {fd, POLLIN, 0};
    char reply[256];
    ssize_t n = ok && poll(&pollfd, 1, 2000) > 0 ? recv(fd, reply, sizeof(reply) - 1, 0) : -1;
    close(fd);
    unlink(local.sun_path);
    if (n > 0) {
        reply[n] = 0;
        if (!quiet)
            fputs(reply, stdout);
        return 0;
    }
    if (!quiet)
        fputs("ClipQueue is not running.\n", stderr);
    return 1;
}
int main(int argc, char **argv) {
    if (!prepare_paths()) {
        fputs("ClipQueue: cannot create private runtime directory.\n", stderr);
        return 1;
    }
    const char *arg = argc > 1 ? argv[1] : "start";
    if (!strcmp(arg, "--daemon"))
        return daemon_main(0, 0, -1);
    if (!strcmp(arg, "--probe"))
        return daemon_main(1, 0, -1);
    if (!strcmp(arg, "--self-test"))
        return daemon_main(0, 1, -1);
    if (!strcmp(arg, "start")) {
        if (!command("status", 1))
            return 0;
        int fds[2];
        if (pipe(fds))
            return 1;
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (!pid) {
            close(fds[0]);
            setsid();
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                if (null > 2)
                    close(null);
            }
            _exit(daemon_main(0, 0, fds[1]));
        }
        close(fds[1]);
        struct pollfd p = {fds[0], POLLIN, 0};
        char ready = 0;
        if (poll(&p, 1, 5000) > 0)
            (void)read(fds[0], &ready, 1);
        close(fds[0]);
        if (!ready)
            fputs("ClipQueue could not start. Run --daemon for details; macOS may need "
                  "Accessibility permission.\n",
                  stderr);
        return ready ? 0 : 1;
    }
    if (strcmp(arg, "status") && strcmp(arg, "on") && strcmp(arg, "off") && strcmp(arg, "toggle") &&
        strcmp(arg, "clear") && strcmp(arg, "stop")) {
        fputs("Usage: clipqueue [start|status|on|off|toggle|clear|stop]\n", stderr);
        return 2;
    }
    return command(arg, 0);
}
