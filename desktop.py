"""GTK clipboard and X11 shortcuts. No network or persistent clipboard history."""
import time
import hashlib
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
from gi.repository import Gtk, Gdk, GLib
from Xlib import X, XK, display, error
from queue_model import Clip


class Desktop:
    def __init__(self, received, status):
        self.received, self.status = received, status
        self.clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        self.x = display.Display()
        self.atom = self.x.intern_atom('CLIPBOARD')
        self.manager_atom = self.x.intern_atom('CLIPBOARD_MANAGER')
        self.last_content = None
        self.last_notification = None
        self.root = self.x.screen().root
        self.collecting = False
        self.generation = 0
        self.own_windows = set()
        self.writing = False
        self.last_replay = 0
        self.hotkeys = {}
        self.clipboard.connect('owner-change', self._changed)
        GLib.timeout_add(15, self._events)

    def owner(self):
        owner = self.x.get_selection_owner(self.atom)
        return getattr(owner, 'id', 0)

    def set_collecting(self, enabled):
        self.collecting = enabled
        self.generation += 1
        self.last_content = None
        self.last_notification = None

    @staticmethod
    def fingerprint(clip):
        if clip.kind == 'text':
            return ('text', hashlib.sha256(clip.data.encode('utf-8')).digest())
        # Ignore row padding and normalize RGB/RGBA differences across owners.
        image = clip.data if clip.data.get_has_alpha() else clip.data.add_alpha(False, 0, 0, 0)
        pixels = memoryview(image.get_pixels())
        stride, width, height = image.get_rowstride(), image.get_width(), image.get_height()
        digest = hashlib.sha256()
        for row in range(height):
            digest.update(pixels[row*stride:row*stride+width*4])
        return ('image', width, height, digest.digest())

    def accept(self, clip, owner):
        signature = self.fingerprint(clip)
        manager = self.x.get_selection_owner(self.manager_atom)
        manager_id = getattr(manager, 'id', 0)
        # The clipboard manager republishes the original content on SAVE_TARGETS.
        # Suppress only that handoff, not a fresh copy from the source app.
        if owner == manager_id and signature == self.last_content:
            return
        self.last_content = signature
        self.received(clip)

    def _changed(self, clipboard, event):
        if not self.collecting or self.writing:
            return
        owner = self.owner()
        if not owner or owner in self.own_windows:
            return
        event_owner = getattr(event.owner, 'id', None)
        if event.owner is not None:
            event_owner = event.owner.get_xid()
        if event_owner and event_owner != owner:
            return
        notification = (owner, event.selection_time)
        if notification == self.last_notification:
            return
        self.last_notification = notification
        self.generation += 1
        generation = self.generation
        def valid():
            return self.collecting and generation == self.generation and self.owner() == owner
        def got_text(cb, text, _):
            if valid() and text:
                self.accept(Clip('text', text, len(text.encode('utf-8')), text), owner)
        def got_image(cb, image, _):
            if not valid():
                return
            if image is not None:
                self.accept(self.image_clip(image), owner)
            else:
                clipboard.request_text(got_text, None)
        clipboard.request_image(got_image, None)

    @staticmethod
    def image_clip(image):
        return Clip('image', image.copy(), image.get_byte_length(),
                    f'{image.get_width()} × {image.get_height()} screenshot / image')

    def write(self, clip):
        self.writing = True
        self.generation += 1
        try:
            if clip.kind == 'image':
                self.clipboard.set_image(clip.data)
            else:
                self.clipboard.set_text(clip.data, -1)
            Gdk.Display.get_default().sync()
            self.own_windows.add(self.owner())
            self.last_content = self.fingerprint(clip)
        finally:
            self.writing = False

    def register(self, key, shift, callback, alt=True, replay=False):
        code = self.x.keysym_to_keycode(XK.string_to_keysym(key))
        mods = X.ControlMask | (X.Mod1Mask if alt else 0) | (X.ShiftMask if shift else 0)
        # Discover Num Lock instead of assuming which modifier it occupies.
        num_code = self.x.keysym_to_keycode(XK.string_to_keysym('Num_Lock'))
        num_mask = 0
        for index, keycodes in enumerate(self.x.get_modifier_mapping()):
            if num_code in keycodes:
                num_mask |= 1 << index
        locks = {0, X.LockMask, num_mask, X.LockMask | num_mask}
        failures = []
        for lock in locks:
            self.root.grab_key(code, mods | lock, False, X.GrabModeAsync, X.GrabModeSync if replay else X.GrabModeAsync,
                               onerror=lambda e, r: failures.append(e))
        self.x.sync()
        if failures:
            for lock in locks:
                self.root.ungrab_key(code, mods | lock)
            raise RuntimeError(f'Ctrl + {"Alt + " if alt else ""}{"Shift + " if shift else ""}{key.upper()} is already in use.')
        self.hotkeys[(code, mods)] = (callback, replay)

    def _events(self):
        try:
            while self.x.pending_events():
                event = self.x.next_event()
                if event.type == X.KeyPress:
                    mods = event.state & (X.ControlMask | X.Mod1Mask | X.ShiftMask | X.Mod4Mask)
                    handler = self.hotkeys.get((event.detail, mods))
                    if handler:
                        callback, replay = handler
                        if replay:
                            delay = max(0, int((self.last_replay + .25 - time.monotonic()) * 1000))
                            GLib.timeout_add(max(1, delay), self._replay, callback)
                        else:
                            callback()
        except (error.XError, OSError) as exc:
            self.status(f'Desktop shortcut error: {exc}')
        return True

    def after_release(self, callback, cancelled=None):
        started = time.monotonic()
        modifier_names = ['Control_L', 'Control_R', 'Alt_L', 'Alt_R', 'Shift_L', 'Shift_R', 'Super_L', 'Super_R']
        codes = [self.x.keysym_to_keycode(XK.string_to_keysym(name)) for name in modifier_names]
        def ready():
            keys = self.x.query_keymap()
            if any(keys[code // 8] & (1 << (code % 8)) for code in codes):
                if time.monotonic() - started > 5:
                    if cancelled:
                        cancelled()
                    self.status('Release the shortcut keys, then try again.')
                    return False
                return True
            callback()
            return False
        GLib.timeout_add(25, ready)

    def _replay(self, callback):
        # Replay the user's actual Ctrl+V after setting the clipboard. This avoids
        # synthetic key recursion and preserves the receiving app's paste binding.
        try:
            callback()
        except Exception as exc:
            self.status(f'Paste error: {exc}')
        finally:
            self.x.allow_events(X.ReplayKeyboard, X.CurrentTime)
            self.x.flush()
            self.last_replay = time.monotonic()
        return False

    def close(self):
        self.x.allow_events(X.AsyncKeyboard, X.CurrentTime)
        self.x.ungrab_keyboard(X.CurrentTime)
        self.x.close()
