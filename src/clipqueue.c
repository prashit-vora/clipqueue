/* ClipQueue: an event-driven, in-memory X11 clipboard queue. */
#include "sha256.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ITEM (64UL * 1024 * 1024)
#define MAX_BYTES (256UL * 1024 * 1024)
#define MAX_ITEMS 200
#define CHUNK 65536UL
#define MAX_OUT 16
#define VERSION "0.2.0"
typedef struct Clip {
    unsigned char *data;
    size_t len;
    Atom target;
    int refs;
    unsigned char hash[32];
} Clip;
typedef struct Node {
    Clip *clip;
    struct Node *next;
} Node;
typedef struct {
    Window win, owner;
    Atom target;
    unsigned char *data;
    size_t len;
    int phase;
    double deadline;
} Capture;
typedef struct {
    Window win;
    Atom prop, target;
    Clip *clip;
    size_t offset;
    double deadline;
} Transfer;
static Display *d;
static Window root, win, osd;
static Atom clipboard, manager, instance, targets, utf8, textplain, textutf8, png, jpeg, incr,
    multiple, timestamp, readprop, statusprop, commandatom;
static Node *head, *tail;
static Clip *current, *last_paste;
static size_t count, bytes;
static int enabled, quiet, running = 1, fixbase, inotifyfd = -1, xerror;
static unsigned char previous[32];
static Atom previous_type;
static int have_previous;
static Capture cap;
static Transfer outgoing[MAX_OUT];
static Time own_time;
static double osd_until, last_replay;
static int paste_pending;
static int toggle_held, clear_held;
static char message[256] = "Ready; queue mode off";
static char toggle_key[128] = "Control+Alt+q", clear_key[128] = "Control+Alt+BackSpace";
static unsigned int numlock_mask;
static KeyCode toggle_code, clear_code, paste_code;
static unsigned int toggle_mod, clear_mod;
static char picture_dirs[3][PATH_MAX];
static int watches[3] = {-1, -1, -1};
static volatile sig_atomic_t stop_requested;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void stop_signal(int s) {
    (void)s;
    stop_requested = 1;
}
static int on_xerror(Display *display, XErrorEvent *e) {
    (void)display;
    xerror = e->error_code;
    return 0;
}
static void status_update(void) {
    char buf[512];
    snprintf(
        buf, sizeof buf,
        "{\"version\":\"%s\",\"enabled\":%s,\"queued\":%zu,\"bytes\":%zu,\"message\":\"%s\"}\n",
        VERSION, enabled ? "true" : "false", count, bytes, message);
    XChangeProperty(d, win, statusprop, XA_STRING, 8, PropModeReplace, (unsigned char *)buf,
                    (int)strlen(buf));
    XFlush(d);
}
static void toast(const char *s) {
    snprintf(message, sizeof message, "%s", s);
    status_update();
    if (quiet)
        return;
    int screen = DefaultScreen(d);
    if (!osd) {
        XSetWindowAttributes a = {
            .override_redirect = True, .background_pixel = 0x19261f, .event_mask = ExposureMask};
        osd = XCreateWindow(d, root, DisplayWidth(d, screen) - 390, 35, 360, 50, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    }
    XMapRaised(d, osd);
    XClearWindow(d, osd);
    GC gc = XCreateGC(d, osd, 0, NULL);
    XSetForeground(d, gc, 0xb7f2cb);
    XDrawString(d, osd, gc, 16, 30, message, (int)strlen(message));
    XFreeGC(d, gc);
    osd_until = now() + 1.5;
}
static Clip *ref(Clip *c) {
    if (c)
        c->refs++;
    return c;
}
static void unref(Clip *c) {
    if (c && !--c->refs) {
        free(c->data);
        free(c);
    }
}
static void reset_previous(void) {
    have_previous = 0;
}
static void clear_queue(void) {
    while (head) {
        Node *n = head;
        head = n->next;
        unref(n->clip);
        free(n);
    }
    tail = NULL;
    count = bytes = 0;
    unref(last_paste);
    last_paste = NULL;
    reset_previous();
}
static void add_clip(Clip *c, int from_manager) {
    if (from_manager && current && current->target == c->target &&
        !memcmp(current->hash, c->hash, 32)) {
        unref(c);
        return;
    }
    if (from_manager && have_previous && previous_type == c->target &&
        !memcmp(previous, c->hash, 32)) {
        unref(c);
        return;
    }
    if (count >= MAX_ITEMS || c->len > MAX_BYTES - bytes) {
        enabled = 0;
        toast("Queue full: paused. Clear or paste items.");
        unref(c);
        return;
    }
    Node *n = calloc(1, sizeof *n);
    if (!n) {
        unref(c);
        toast("Out of memory: copy not added");
        return;
    }
    n->clip = c;
    if (tail)
        tail->next = n;
    else
        head = n;
    tail = n;
    count++;
    bytes += c->len;
    memcpy(previous, c->hash, 32);
    previous_type = c->target;
    have_previous = 1;
    snprintf(message, sizeof message, "Added %s; %zu queued",
             c->target == png || c->target == jpeg ? "image" : "text", count);
    status_update();
}
static Clip *new_clip(unsigned char *data, size_t len, Atom target) {
    Clip *c = calloc(1, sizeof *c);
    if (!c) {
        free(data);
        return NULL;
    }
    c->data = data;
    c->len = len;
    c->target = target;
    c->refs = 1;
    sha256(data, len, c->hash);
    return c;
}
static void abort_capture(void) {
    if (cap.win)
        XDestroyWindow(d, cap.win);
    free(cap.data);
    memset(&cap, 0, sizeof cap);
}
static void finish_capture(void) {
    if (!cap.len || !enabled) {
        abort_capture();
        return;
    }
    Clip *c = new_clip(cap.data, cap.len, cap.target);
    cap.data = NULL;
    int is_manager = cap.owner == XGetSelectionOwner(d, manager);
    abort_capture();
    if (c)
        add_clip(c, is_manager);
}
static void request_data(Atom target) {
    cap.target = target;
    cap.phase = 2;
    XConvertSelection(d, clipboard, target, readprop, cap.win, CurrentTime);
    XFlush(d);
}
static int append_data(const unsigned char *data, size_t len) {
    if (len > MAX_ITEM - cap.len)
        return 0;
    unsigned char *p = realloc(cap.data, cap.len + len);
    if (!p && len)
        return 0;
    cap.data = p;
    if (len)
        memcpy(cap.data + cap.len, data, len);
    cap.len += len;
    return 1;
}
static void start_capture(Window owner) {
    if (!enabled || owner == None || owner == win)
        return;
    if (owner != XGetSelectionOwner(d, clipboard))
        return;
    abort_capture();
    cap.owner = owner;
    cap.win = XCreateSimpleWindow(d, root, -1, -1, 1, 1, 0, 0, 0);
    XSelectInput(d, cap.win, PropertyChangeMask);
    cap.phase = 1;
    cap.deadline = now() + 10;
    XConvertSelection(d, clipboard, targets, readprop, cap.win, CurrentTime);
    XFlush(d);
}
static void selection_notify(XSelectionEvent *e) {
    if (e->requestor != cap.win || !cap.win)
        return;
    if (e->property == None) {
        abort_capture();
        return;
    }
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, cap.win, readprop, 0, MAX_ITEM / 4 + 1, True, AnyPropertyType, &type,
                           &format, &n, &after, &data) != Success) {
        abort_capture();
        return;
    }
    if (after) {
        if (data)
            XFree(data);
        abort_capture();
        toast("Clipboard item exceeds 64 MB limit");
        return;
    }
    if (cap.phase == 1) {
        Atom chosen = None;
        Atom prefs[] = {png, jpeg, utf8, textutf8, textplain, XA_STRING};
        if (type == XA_ATOM && format == 32)
            for (size_t i = 0; i < sizeof prefs / sizeof prefs[0] && !chosen; i++)
                for (unsigned long j = 0; j < n; j++)
                    if (((Atom *)data)[j] == prefs[i]) {
                        chosen = prefs[i];
                        break;
                    }
        if (data)
            XFree(data);
        if (chosen)
            request_data(chosen);
        else
            abort_capture();
        return;
    }
    if (type == incr) {
        cap.phase = 3;
        cap.deadline = now() + 10;
        XDeleteProperty(d, cap.win, readprop);
        XFlush(d);
    } else if (format == 8 && append_data(data, n)) {
        finish_capture();
    } else
        abort_capture();
    if (data)
        XFree(data);
}
static void receive_chunk(void) {
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, cap.win, readprop, 0, MAX_ITEM / 4 + 1, True, AnyPropertyType, &type,
                           &format, &n, &after, &data) != Success)
        return;
    if (type == None) {
        if (data)
            XFree(data);
        return;
    }
    if (after || format != 8 || !append_data(data, n)) {
        if (data)
            XFree(data);
        abort_capture();
        toast("Clipboard transfer too large or invalid");
        return;
    }
    if (data)
        XFree(data);
    cap.deadline = now() + 10;
    if (!n)
        finish_capture();
}
static void close_transfer(Transfer *t) {
    unref(t->clip);
    memset(t, 0, sizeof *t);
}
static int send_target(Window requestor, Atom target, Atom prop) {
    if (!current || !prop)
        return 0;
    if (target == targets) {
        Atom list[8] = {targets, timestamp, multiple, current->target};
        int n = 4;
        if (current->target == utf8 || current->target == textutf8 ||
            current->target == textplain || current->target == XA_STRING) {
            list[n++] = utf8;
            list[n++] = textutf8;
            list[n++] = textplain;
        }
        XChangeProperty(d, requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, n);
        return 1;
    }
    if (target == timestamp) {
        unsigned long time = own_time;
        XChangeProperty(d, requestor, prop, XA_INTEGER, 32, PropModeReplace, (unsigned char *)&time,
                        1);
        return 1;
    }
    int istext = current->target != png && current->target != jpeg;
    if (target != current->target &&
        !(istext && (target == utf8 || target == textutf8 || target == textplain)))
        return 0;
    if (current->len <= CHUNK) {
        XChangeProperty(d, requestor, prop, target, 8, PropModeReplace, current->data,
                        (int)current->len);
        return 1;
    }
    for (int i = 0; i < MAX_OUT; i++)
        if (!outgoing[i].clip) {
            Transfer *t = &outgoing[i];
            *t = (Transfer){.win = requestor,
                            .prop = prop,
                            .target = target,
                            .clip = ref(current),
                            .deadline = now() + 15};
            XSelectInput(d, requestor, PropertyChangeMask | StructureNotifyMask);
            unsigned long len = current->len;
            XChangeProperty(d, requestor, prop, incr, 32, PropModeReplace, (unsigned char *)&len,
                            1);
            return 1;
        }
    return 0;
}
static void selection_request(XSelectionRequestEvent *r) {
    XEvent reply = {0};
    reply.xselection = (XSelectionEvent){.type = SelectionNotify,
                                         .display = d,
                                         .requestor = r->requestor,
                                         .selection = r->selection,
                                         .target = r->target,
                                         .time = r->time,
                                         .property = None};
    Atom prop = r->property ? r->property : r->target;
    if (r->selection == clipboard && r->target == multiple && r->property) {
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(d, r->requestor, r->property, 0, 2048, False, AnyPropertyType, &type,
                               &format, &n, &after, &data) == Success &&
            format == 32 && !after && n % 2 == 0) {
            Atom *pairs = (Atom *)data;
            for (unsigned long i = 0; i < n; i += 2)
                if (pairs[i] == multiple || !send_target(r->requestor, pairs[i], pairs[i + 1]))
                    pairs[i + 1] = None;
            XChangeProperty(d, r->requestor, r->property, type, 32, PropModeReplace, data, (int)n);
            reply.xselection.property = r->property;
        }
        if (data)
            XFree(data);
    } else if (r->selection == clipboard && send_target(r->requestor, r->target, prop))
        reply.xselection.property = prop;
    XSendEvent(d, r->requestor, False, 0, &reply);
    XFlush(d);
}
static void property_event(XPropertyEvent *e) {
    if (cap.win && e->window == cap.win && e->atom == readprop && e->state == PropertyNewValue &&
        cap.phase == 3) {
        receive_chunk();
        return;
    }
    if (e->state != PropertyDelete)
        return;
    for (int i = 0; i < MAX_OUT; i++) {
        Transfer *t = &outgoing[i];
        if (!t->clip || e->window != t->win || e->atom != t->prop)
            continue;
        size_t n = t->clip->len - t->offset;
        if (n > CHUNK)
            n = CHUNK;
        XChangeProperty(d, t->win, t->prop, t->target, 8, PropModeReplace,
                        t->clip->data + t->offset, (int)n);
        t->offset += n;
        t->deadline = now() + 15;
        if (!n)
            close_transfer(t);
    }
    XFlush(d);
}
static void paste_next(void) {
    if (enabled && head) {
        Clip *c = head->clip;
        XSetSelectionOwner(d, clipboard, win, CurrentTime);
        XSync(d, False);
        if (XGetSelectionOwner(d, clipboard) == win) {
            abort_capture();
            unref(current);
            current = ref(c);
            unref(last_paste);
            last_paste = ref(c);
            Node *n = head;
            head = n->next;
            if (!head)
                tail = NULL;
            count--;
            bytes -= c->len;
            unref(n->clip);
            free(n);
            snprintf(message, sizeof message, "Pasted next item; %zu queued", count);
            status_update();
        }
    }
    XAllowEvents(d, ReplayKeyboard, CurrentTime);
    XFlush(d);
    last_replay = now();
    paste_pending = 0;
}
static void set_mode(int value) {
    enabled = value;
    abort_capture();
    reset_previous();
    toast(enabled ? "ClipQueue ON" : "ClipQueue OFF - normal clipboard");
}
static void undo(void) {
    if (!last_paste) {
        toast("No last paste to restore");
        return;
    }
    if (count >= MAX_ITEMS || last_paste->len > MAX_BYTES - bytes) {
        toast("Queue full; make room before restoring");
        return;
    }
    Node *n = calloc(1, sizeof *n);
    if (!n)
        return;
    n->clip = last_paste;
    last_paste = NULL;
    n->next = head;
    head = n;
    if (!tail)
        tail = n;
    count++;
    bytes += n->clip->len;
    reset_previous();
    toast("Last paste restored to queue");
}
static void command(int code) {
    switch (code) {
    case 1:
        set_mode(1);
        break;
    case 2:
        set_mode(0);
        break;
    case 3:
        set_mode(!enabled);
        break;
    case 4:
        clear_queue();
        abort_capture();
        toast("Queue cleared");
        break;
    case 5:
        undo();
        break;
    case 6:
        running = 0;
        break;
    default:
        break;
    }
}
static int parse_key(const char *s, KeyCode *code, unsigned int *mods) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    *mods = 0;
    char *save = NULL, *part = strtok_r(buf, "+", &save);
    KeySym symbol = NoSymbol;
    while (part) {
        if (!strcasecmp(part, "Control") || !strcasecmp(part, "Ctrl"))
            *mods |= ControlMask;
        else if (!strcasecmp(part, "Alt"))
            *mods |= Mod1Mask;
        else if (!strcasecmp(part, "Shift"))
            *mods |= ShiftMask;
        else if (!strcasecmp(part, "Super") || !strcasecmp(part, "Win"))
            *mods |= Mod4Mask;
        else
            symbol = XStringToKeysym(part);
        part = strtok_r(NULL, "+", &save);
    }
    *code = XKeysymToKeycode(d, symbol);
    return *code != 0;
}
static int grab(KeyCode code, unsigned int mods, int sync) {
    unsigned locks[] = {0, LockMask, numlock_mask, LockMask | numlock_mask};
    xerror = 0;
    for (int i = 0; i < 4; i++)
        XGrabKey(d, code, mods | locks[i], root, False, GrabModeAsync,
                 sync ? GrabModeSync : GrabModeAsync);
    XSync(d, False);
    return !xerror;
}
static void load_config(void) {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[PATH_MAX];
    if (config)
        snprintf(path, sizeof path, "%s/clipqueue/config", config);
    else
        snprintf(path, sizeof path, "%s/.config/clipqueue/config", home ? home : ".");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        if (!v || line[0] == '#')
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "toggle_key"))
            snprintf(toggle_key, sizeof toggle_key, "%.127s", v);
        if (!strcmp(line, "clear_key"))
            snprintf(clear_key, sizeof clear_key, "%.127s", v);
        if (!strcmp(line, "quiet"))
            quiet = !strcmp(v, "true");
        if (!strcmp(line, "screenshot_dir"))
            snprintf(picture_dirs[2], PATH_MAX, "%s", v);
    }
    fclose(f);
}
static void setup_watches(void) {
    const char *home = getenv("HOME");
    if (!home)
        return;
    snprintf(picture_dirs[0], PATH_MAX, "%s/Pictures", home);
    snprintf(picture_dirs[1], PATH_MAX, "%s/Pictures/Screenshots", home);
    inotifyfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyfd < 0)
        return;
    for (int i = 0; i < 3; i++)
        if (picture_dirs[i][0])
            watches[i] =
                inotify_add_watch(inotifyfd, picture_dirs[i], IN_CLOSE_WRITE | IN_MOVED_TO);
}
static void saved_screenshot(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long)st.st_size > MAX_ITEM) {
        close(fd);
        return;
    }
    size_t len = (size_t)st.st_size;
    unsigned char *data = malloc(len);
    if (!data) {
        close(fd);
        return;
    }
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = read(fd, data + offset, len - offset);
        if (n <= 0)
            break;
        offset += (size_t)n;
    }
    close(fd);
    if (offset != len || len < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        free(data);
        return;
    }
    Clip *c = new_clip(data, len, png);
    if (c)
        add_clip(c, 0);
}
static void file_events(void) {
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    while ((n = read(inotifyfd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *e = (void *)p;
            if (enabled && e->len && !strncasecmp(e->name, "Screenshot", 10)) {
                for (int i = 0; i < 3; i++)
                    if (e->wd == watches[i]) {
                        char path[PATH_MAX];
                        if (snprintf(path, sizeof path, "%s/%s", picture_dirs[i], e->name) <
                            (int)sizeof path)
                            saved_screenshot(path);
                        break;
                    }
            }
            p += sizeof *e + e->len;
        }
    }
}
static void init_atoms(void) {
#define A(var, name) var = XInternAtom(d, name, False)
    A(clipboard, "CLIPBOARD");
    A(manager, "CLIPBOARD_MANAGER");
    A(instance, "_CLIPQUEUE_DAEMON");
    A(targets, "TARGETS");
    A(utf8, "UTF8_STRING");
    A(textplain, "text/plain");
    A(textutf8, "text/plain;charset=utf-8");
    A(png, "image/png");
    A(jpeg, "image/jpeg");
    A(incr, "INCR");
    A(multiple, "MULTIPLE");
    A(timestamp, "TIMESTAMP");
    A(readprop, "_CLIPQUEUE_READ");
    A(statusprop, "_CLIPQUEUE_STATUS");
    A(commandatom, "_CLIPQUEUE_COMMAND");
#undef A
}
static int read_status(Window w) {
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    int ok = XGetWindowProperty(d, w, statusprop, 0, 2048, False, XA_STRING, &type, &fmt, &n,
                                &after, &p) == Success &&
             fmt == 8;
    if (ok)
        fwrite(p, 1, n, stdout);
    if (p)
        XFree(p);
    return ok ? 0 : 1;
}
static int client(const char *cmd) {
    Window server = XGetSelectionOwner(d, instance);
    if (!server) {
        fprintf(stderr, "ClipQueue is not running. Use: clipqueue start\n");
        return 1;
    }
    if (!strcmp(cmd, "status"))
        return read_status(server);
    const char *commands[] = {"on", "off", "toggle", "clear", "undo", "stop"};
    int code = 0;
    for (int i = 0; i < 6; i++)
        if (!strcmp(cmd, commands[i]))
            code = i + 1;
    if (!code) {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    Window reply = XCreateSimpleWindow(d, root, -1, -1, 1, 1, 0, 0, 0);
    XSelectInput(d, reply, PropertyChangeMask);
    XEvent e = {0};
    e.xclient = (XClientMessageEvent){.type = ClientMessage,
                                      .display = d,
                                      .window = server,
                                      .message_type = commandatom,
                                      .format = 32};
    e.xclient.data.l[0] = code;
    e.xclient.data.l[1] = (long)reply;
    XSendEvent(d, server, False, 0, &e);
    XFlush(d);
    double deadline = now() + 3;
    while (now() < deadline) {
        if (XPending(d)) {
            XNextEvent(d, &e);
            if (e.type == PropertyNotify && e.xproperty.window == reply &&
                e.xproperty.atom == statusprop) {
                int ret = read_status(reply);
                XDestroyWindow(d, reply);
                return ret;
            }
        } else {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(d), &fds);
            struct timeval tv = {.tv_usec = 100000};
            select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
        }
    }
    fprintf(stderr, "Daemon did not acknowledge command\n");
    XDestroyWindow(d, reply);
    return 1;
}
static int daemon_init(void) {
    int errorbase, major = 2, minor = 0;
    if (!XFixesQueryExtension(d, &fixbase, &errorbase) || !XFixesQueryVersion(d, &major, &minor)) {
        fprintf(stderr, "XFixes is required\n");
        return 1;
    }
    XGrabServer(d);
    if (XGetSelectionOwner(d, instance)) {
        XUngrabServer(d);
        fprintf(stderr, "ClipQueue is already running\n");
        return 1;
    }
    win = XCreateSimpleWindow(d, root, -1, -1, 1, 1, 0, 0, 0);
    XSetSelectionOwner(d, instance, win, CurrentTime);
    XUngrabServer(d);
    XSelectInput(d, win, PropertyChangeMask);
    XFixesSelectSelectionInput(d, win, clipboard,
                               XFixesSetSelectionOwnerNotifyMask |
                                   XFixesSelectionWindowDestroyNotifyMask |
                                   XFixesSelectionClientCloseNotifyMask);
    XModifierKeymap *map = XGetModifierMapping(d);
    KeyCode num = XKeysymToKeycode(d, XK_Num_Lock);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < map->max_keypermod; j++)
            if (map->modifiermap[i * map->max_keypermod + j] == num)
                numlock_mask |= 1U << i;
    XFreeModifiermap(map);
    paste_code = XKeysymToKeycode(d, XK_v);
    if (!parse_key(toggle_key, &toggle_code, &toggle_mod) ||
        !parse_key(clear_key, &clear_code, &clear_mod) || !toggle_mod || !clear_mod ||
        (toggle_code == paste_code && toggle_mod == ControlMask) ||
        (clear_code == paste_code && clear_mod == ControlMask) ||
        (toggle_code == clear_code && toggle_mod == clear_mod)) {
        fprintf(stderr, "Invalid or conflicting queue-control shortcuts in config\n");
        return 1;
    }
    if (!grab(paste_code, ControlMask, 1) || !grab(toggle_code, toggle_mod, 0) ||
        !grab(clear_code, clear_mod, 0)) {
        fprintf(
            stderr,
            "A shortcut is in use. Change toggle_key/clear_key in ~/.config/clipqueue/config.\n");
        return 1;
    }
    setup_watches();
    status_update();
    return 0;
}
static void dispatch(XEvent *e) {
    if (e->type == fixbase + XFixesSelectionNotify) {
        XFixesSelectionNotifyEvent *f = (void *)e;
        if (f->selection == clipboard) {
            if (f->owner == win)
                own_time = f->selection_timestamp;
            else
                start_capture(f->owner);
        }
        return;
    }
    switch (e->type) {
    case SelectionNotify:
        selection_notify(&e->xselection);
        break;
    case SelectionRequest:
        selection_request(&e->xselectionrequest);
        break;
    case PropertyNotify:
        property_event(&e->xproperty);
        break;
    case DestroyNotify:
        for (int i = 0; i < MAX_OUT; i++)
            if (outgoing[i].clip && outgoing[i].win == e->xdestroywindow.window)
                close_transfer(&outgoing[i]);
        break;
    case KeyPress: {
        unsigned m = e->xkey.state & ~(numlock_mask | LockMask);
        if (e->xkey.keycode == paste_code && m == ControlMask) {
            paste_pending = 1;
            if (now() - last_replay >= .2)
                paste_next();
        } else if (e->xkey.keycode == toggle_code && m == toggle_mod && !toggle_held) {
            toggle_held = 1;
            set_mode(!enabled);
        } else if (e->xkey.keycode == clear_code && m == clear_mod && !clear_held) {
            clear_held = 1;
            clear_queue();
            abort_capture();
            toast("Queue cleared");
        }
        break;
    }
    case KeyRelease:
        if (e->xkey.keycode == toggle_code)
            toggle_held = 0;
        if (e->xkey.keycode == clear_code)
            clear_held = 0;
        break;
    case ClientMessage:
        if (e->xclient.message_type == commandatom) {
            command((int)e->xclient.data.l[0]);
            Window reply = (Window)e->xclient.data.l[1];
            if (reply) {
                Atom type;
                int format;
                unsigned long n, after;
                unsigned char *p = NULL;
                if (XGetWindowProperty(d, win, statusprop, 0, 2048, False, XA_STRING, &type,
                                       &format, &n, &after, &p) == Success &&
                    format == 8)
                    XChangeProperty(d, reply, statusprop, XA_STRING, 8, PropModeReplace, p, (int)n);
                if (p)
                    XFree(p);
                XFlush(d);
            }
        }
        break;
    case Expose:
        if (e->xexpose.window == osd) {
            GC gc = XCreateGC(d, osd, 0, NULL);
            XSetForeground(d, gc, 0xb7f2cb);
            XDrawString(d, osd, gc, 16, 30, message, (int)strlen(message));
            XFreeGC(d, gc);
        }
        break;
    default:
        break;
    }
}
static void preserve_clipboard(void) {
    if (!current || XGetSelectionOwner(d, clipboard) != win || !XGetSelectionOwner(d, manager))
        return;
    Atom save = XInternAtom(d, "SAVE_TARGETS", False),
         prop = XInternAtom(d, "_CLIPQUEUE_SAVE", False);
    XConvertSelection(d, manager, save, prop, win, CurrentTime);
    XFlush(d);
    double until = now() + 1.5;
    while (now() < until) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == SelectionNotify && e.xselection.selection == manager)
                return;
            if (e.type == SelectionRequest)
                selection_request(&e.xselectionrequest);
            else if (e.type == PropertyNotify)
                property_event(&e.xproperty);
        }
        struct timespec wait = {0, 10000000};
        nanosleep(&wait, NULL);
    }
}
static int run_daemon(int readyfd) {
    if (daemon_init()) {
        if (readyfd >= 0) {
            char r = '0';
            if (write(readyfd, &r, 1) != 1) {
            }
            close(readyfd);
        }
        return 1;
    }
    if (readyfd >= 0) {
        char r = '1';
        if (write(readyfd, &r, 1) != 1) {
        }
        close(readyfd);
    }
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    while (running && !stop_requested) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            dispatch(&e);
        }
        double t = now();
        if (paste_pending && t - last_replay >= .2)
            paste_next();
        if (cap.win && t > cap.deadline) {
            abort_capture();
            toast("Clipboard transfer timed out; copy again");
        }
        for (int i = 0; i < MAX_OUT; i++)
            if (outgoing[i].clip && t > outgoing[i].deadline)
                close_transfer(&outgoing[i]);
        if (osd && osd_until && t > osd_until) {
            XUnmapWindow(d, osd);
            osd_until = 0;
        }
        fd_set fds;
        FD_ZERO(&fds);
        int xfd = ConnectionNumber(d), maxfd = xfd;
        FD_SET(xfd, &fds);
        if (inotifyfd >= 0) {
            FD_SET(inotifyfd, &fds);
            if (inotifyfd > maxfd)
                maxfd = inotifyfd;
        }
        struct timeval tv = {.tv_usec = paste_pending ? 20000 : 200000};
        if (select(maxfd + 1, &fds, NULL, NULL, &tv) > 0 && inotifyfd >= 0 &&
            FD_ISSET(inotifyfd, &fds))
            file_events();
    }
    XAllowEvents(d, AsyncKeyboard, CurrentTime);
    XUngrabKeyboard(d, CurrentTime);
    preserve_clipboard();
    abort_capture();
    clear_queue();
    unref(current);
    for (int i = 0; i < MAX_OUT; i++)
        close_transfer(&outgoing[i]);
    if (inotifyfd >= 0)
        close(inotifyfd);
    return 0;
}
int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "start";
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        puts("ClipQueue " VERSION
             " (Linux X11)\nUsage: clipqueue "
             "start|--daemon|status|on|off|toggle|clear|undo|stop\nControls: Ctrl+Alt+Q toggle; "
             "Ctrl+Alt+Backspace clear; Ctrl+V FIFO paste.\nConfig: ~/.config/clipqueue/config. "
             "Starts paused. No clipboard history on disk.");
        return 0;
    }
    if (!strcmp(cmd, "--version")) {
        puts(VERSION);
        return 0;
    }
    const char *session = getenv("XDG_SESSION_TYPE");
    if (session && !strcmp(session, "wayland")) {
        fprintf(stderr, "This release requires an X11 session; Wayland is not supported.\n");
        return 1;
    }
    load_config();
    int readyfd = -1;
    if (!strcmp(cmd, "start")) {
        int pipefd[2];
        if (pipe(pipefd))
            return 1;
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (pid) {
            close(pipefd[1]);
            char r = 0;
            ssize_t n = read(pipefd[0], &r, 1);
            close(pipefd[0]);
            if (n == 1 && r == '1') {
                puts("ClipQueue started in background (queue mode off).");
                return 0;
            }
            waitpid(pid, NULL, 0);
            fprintf(stderr, "Could not start ClipQueue. Run clipqueue --daemon for details.\n");
            return 1;
        }
        close(pipefd[0]);
        readyfd = pipefd[1];
        setsid();
    }
    d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "Cannot connect to X11 DISPLAY\n");
        return 1;
    }
    root = DefaultRootWindow(d);
    XSetErrorHandler(on_xerror);
    init_atoms();
    int result;
    if (!strcmp(cmd, "start") || !strcmp(cmd, "--daemon")) {
        if (readyfd >= 0) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                if (null > 2)
                    close(null);
            }
        }
        result = run_daemon(readyfd);
    } else
        result = client(cmd);
    XCloseDisplay(d);
    return result;
}
