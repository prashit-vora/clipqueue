#!/usr/bin/env python3
"""ClipQueue — collect now, paste in order."""
import os
import sys
from pathlib import Path
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
from gi.repository import Gtk, Gdk, GdkPixbuf, Gio, GLib, Pango
from queue_model import ClipQueue
from desktop import Desktop
from screenshot import RegionPicker
from saved_screenshots import ScreenshotWatcher

BASE = Path(__file__).resolve().parent
CSS = b'''
window { background: #141918; color: #edf2ed; }
.header { font-size: 28px; font-weight: 800; letter-spacing: -1px; }
.subtle { color: #a6b3ae; font-size: 13px; }
.eyebrow { color: #8bd5ac; font-size: 11px; font-weight: 700; letter-spacing: 2px; }
.card { background: #202925; border-radius: 14px; padding: 18px; }
.count { color: #a9edc5; font-size: 38px; font-weight: 700; }
button { background: #2b3932; color: #edf2ed; border: 1px solid #405047; border-radius: 8px; padding: 9px 13px; box-shadow: none; text-shadow: none; }
button:hover { background: #3a4a41; }
button.primary { background: #a9edc5; color: #14231b; border: none; font-weight: 700; }
button:disabled { opacity: .4; }
switch:checked { background: #82cfa2; }
list { background: transparent; }
row { background: #202925; border-radius: 10px; margin-bottom: 8px; padding: 12px; }
row:hover { background: #29342e; }
.preview { color: #edf2ed; font-size: 14px; }
.empty { color: #afbeb6; font-size: 16px; }
.status { color: #b6c7bd; font-size: 12px; }
'''


def label(text, css=None):
    widget = Gtk.Label(label=text, xalign=0)
    if css:
        widget.get_style_context().add_class(css)
    return widget


def button(text, callback, primary=False):
    widget = Gtk.Button(label=text)
    widget.connect('clicked', callback)
    if primary:
        widget.get_style_context().add_class('primary')
    return widget


class App(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='local.clipqueue.Desktop', flags=Gio.ApplicationFlags.FLAGS_NONE)
        self.queue = ClipQueue()
        self.window = None
        self.desktop = None
        self.picker = None

    def do_activate(self):
        if self.window:
            self.window.present()
            return
        if os.environ.get('XDG_SESSION_TYPE') == 'wayland' or not isinstance(Gdk.Display.get_default(), __import__('gi.repository.GdkX11', fromlist=['X11Display']).X11Display):
            self.failure('This version needs a Linux X11 session. Wayland, Windows and macOS backends are not included yet.')
            return
        try:
            self.desktop = Desktop(self.receive, self.status)
        except Exception as exc:
            self.failure(str(exc))
            return
        self.watcher = ScreenshotWatcher(lambda image: self.receive(self.desktop.image_clip(image)), self.status)
        self.build()
        try:
            self.desktop.register('v', False, self.paste_next, alt=False, replay=True)
        except Exception as exc:
            self.failure(str(exc))
            self.quit()
            return
        self.hold()
        self.window.show_all()
        self.refresh()

    def failure(self, message):
        dialog = Gtk.MessageDialog(message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE,
                                   text='ClipQueue could not start')
        dialog.format_secondary_text(message)
        dialog.run()
        dialog.destroy()

    def build(self):
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.window = Gtk.ApplicationWindow(application=self, title='ClipQueue')
        self.window.set_default_size(560, 660)
        self.window.set_position(Gtk.WindowPosition.CENTER)
        self.window.set_icon_from_file(str(BASE / 'icon.svg'))
        self.window.connect('delete-event', self.hide_window)
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=17, margin=24)
        self.window.add(outer)
        outer.pack_start(label('COPY LESS OF THE ROUTINE', 'eyebrow'), False, False, 0)
        top = Gtk.Box(spacing=12)
        titles = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5)
        titles.pack_start(label('ClipQueue', 'header'), False, False, 0)
        titles.pack_start(label('Collect now. Paste in order.', 'subtle'), False, False, 0)
        top.pack_start(titles, True, True, 0)
        self.mode = Gtk.Switch(valign=Gtk.Align.CENTER)
        self.mode.set_tooltip_text('Queue mode: collect copies and paste them in order with Ctrl + V')
        self.mode.connect('notify::active', self.mode_changed)
        top.pack_start(self.mode, False, False, 0)
        outer.pack_start(top, False, False, 0)
        card = Gtk.Box(spacing=20)
        card.get_style_context().add_class('card')
        self.count = label('0', 'count')
        card.pack_start(self.count, False, False, 0)
        detail = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        self.mode_label = label('Queue mode is off')
        detail.pack_start(self.mode_label, False, False, 0)
        detail.pack_start(label('Ctrl + V  →  paste the oldest item', 'subtle'), False, False, 0)
        detail.pack_start(label('Your copy and screenshot shortcuts stay the same', 'subtle'), False, False, 0)
        card.pack_start(detail, True, True, 0)
        outer.pack_start(card, False, False, 0)
        actions = Gtk.Box(spacing=8)
        actions.pack_start(button('＋  Capture area', lambda _: self.screenshot(), True), True, True, 0)
        self.restore = button('Restore last paste', self.undo)
        actions.pack_start(self.restore, False, False, 0)
        actions.pack_start(button('Clear', self.clear), False, False, 0)
        outer.pack_start(actions, False, False, 0)
        heading = Gtk.Box()
        heading.pack_start(label('YOUR QUEUE', 'eyebrow'), True, True, 0)
        heading.pack_start(label('First in → first out', 'subtle'), False, False, 0)
        outer.pack_start(heading, False, False, 0)
        self.stack = Gtk.Stack()
        self.stack.set_vexpand(True)
        empty = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14, valign=Gtk.Align.CENTER)
        empty.pack_start(label('A little breathing room for your clipboard.', 'empty'), False, False, 0)
        instructions = label('1   Turn queue mode on.\n2   Copy text or images, or capture a few areas.\n3   Click your destination and press Ctrl + V for each item.', 'subtle')
        instructions.set_line_wrap(True)
        empty.pack_start(instructions, False, False, 0)
        self.stack.add_named(empty, 'empty')
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.rows = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        scroll.add(self.rows)
        self.stack.add_named(scroll, 'queue')
        outer.pack_start(self.stack, True, True, 0)
        self.status_label = label('Ready. Turn on queue mode to start collecting.', 'status')
        self.status_label.set_line_wrap(True)
        self.status_label.set_max_width_chars(64)
        outer.pack_start(self.status_label, False, False, 0)
        footer = Gtk.Box(spacing=12)
        footer.pack_start(label('In memory only · Cleared when you quit', 'subtle'), True, True, 0)
        footer.pack_start(button('Quit', lambda _: self.quit()), False, False, 0)
        outer.pack_start(footer, False, False, 0)
        # Cinnamon supports a tray icon; the launcher also brings the window back.
        self.tray = Gtk.StatusIcon.new_from_file(str(BASE / 'icon.svg'))
        self.tray.set_tooltip_text('ClipQueue — queue mode off')
        self.tray.connect('activate', lambda _: self.window.present())
        self.tray.connect('popup-menu', self.tray_menu)
        self.tray.set_visible(True)

    def hide_window(self, *_):
        if self.tray.is_embedded():
            self.window.hide()
        else:
            self.window.iconify()
        return True

    def tray_menu(self, icon, mouse_button, time):
        menu = Gtk.Menu()
        for title, action in [('Open ClipQueue', self.window.present),
                              ('Turn queue mode off' if self.mode.get_active() else 'Turn queue mode on', self.toggle),
                              ('Capture area', self.screenshot), ('Quit', self.quit)]:
            item = Gtk.MenuItem(label=title)
            item.connect('activate', lambda _, fn=action: fn())
            menu.append(item)
        menu.show_all()
        menu.popup(None, None, Gtk.StatusIcon.position_menu, icon, mouse_button, time)

    def toggle(self):
        self.mode.set_active(not self.mode.get_active())

    def mode_changed(self, *_):
        enabled = self.mode.get_active()
        self.desktop.set_collecting(enabled)
        self.watcher.set_enabled(enabled)
        self.mode_label.set_text('Collecting · Ctrl + V pastes in order' if enabled else 'Queue mode is off · Normal clipboard')
        self.status('Collecting new copies. Click your destination and use Ctrl + V.' if enabled else 'Paused. Your queued items are kept; copying and pasting work normally.')
        self.refresh()

    def receive(self, clip):
        try:
            self.queue.append(clip)
            self.status(f'Added {"image" if clip.kind == "image" else "text"}. {len(self.queue.items)} in queue.')
        except ValueError as exc:
            self.mode.set_active(False)
            self.status(str(exc))
        self.refresh()

    def paste_next(self):
        if not self.mode.get_active() or not self.queue.peek():
            return
        clip = self.queue.peek()
        self.desktop.write(clip)
        self.queue.commit(clip.id)
        self.status(f'Sent next item to paste. {len(self.queue.items)} left. Restore last paste if the app did not accept it.')
        self.refresh()

    def undo(self, *_):
        try:
            if self.queue.undo():
                self.status('Last item restored to the front. This does not undo text in the destination app.')
        except ValueError as exc:
            self.status(str(exc))
        self.refresh()

    def clear(self, *_):
        self.queue.clear()
        self.status('Queue cleared. The system clipboard still holds its current item.')
        self.refresh()

    def remove(self, clip_id):
        self.queue.remove(clip_id)
        self.refresh()

    def refresh(self):
        if not self.window:
            return
        self.count.set_text(str(len(self.queue.items)))
        self.restore.set_sensitive(self.queue.last_pasted is not None)
        self.tray.set_tooltip_text(f'ClipQueue — {len(self.queue.items)} queued — {"on" if self.mode.get_active() else "off"}')
        for child in self.rows.get_children():
            self.rows.remove(child)
        for index, clip in enumerate(self.queue.items):
            row = Gtk.Box(spacing=12)
            number = label(f'{index+1:02}', 'eyebrow')
            number.set_size_request(26,-1)
            row.pack_start(number, False, False, 0)
            if clip.kind == 'image':
                image = clip.data
                ratio = min(88/image.get_width(),56/image.get_height(),1)
                thumb = image.scale_simple(max(1,int(image.get_width()*ratio)),max(1,int(image.get_height()*ratio)),GdkPixbuf.InterpType.BILINEAR)
                row.pack_start(Gtk.Image.new_from_pixbuf(thumb),False,False,0)
            content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL,spacing=5)
            content.pack_start(label(('UP NEXT · ' if index == 0 else '') + clip.kind.upper(),'eyebrow'),False,False,0)
            preview = label(' '.join(clip.label.split())[:140], 'preview')
            preview.set_ellipsize(Pango.EllipsizeMode.END)
            preview.set_max_width_chars(30)
            content.pack_start(preview,False,False,0)
            row.pack_start(content,True,True,0)
            remove = button('×',lambda _, clip_id=clip.id: self.remove(clip_id))
            remove.set_tooltip_text('Remove this item')
            row.pack_start(remove,False,False,0)
            self.rows.add(row)
        self.rows.show_all()
        self.stack.set_visible_child_name('queue' if self.queue.items else 'empty')

    def status(self, text):
        if hasattr(self, 'status_label'):
            self.status_label.set_text(text)

    def screenshot(self):
        if self.picker:
            return
        self.mode.set_active(True)
        self.window.hide()
        def capture():
            def begin():
                try:
                    self.picker = RegionPicker(self.captured)
                except Exception as exc:
                    self.status(str(exc))
                    self.window.present()
                return False
            GLib.timeout_add(180,begin)
        self.desktop.after_release(capture)

    def captured(self, image):
        self.picker = None
        if image is not None:
            self.receive(self.desktop.image_clip(image))
        else:
            self.status('Capture cancelled.')

    def do_shutdown(self):
        if self.picker:
            self.picker.finish(None)
        if hasattr(self, 'watcher'):
            self.watcher.close()
        if self.desktop:
            self.desktop.close()
        self.queue.clear()
        Gtk.Application.do_shutdown(self)


if __name__ == '__main__':
    sys.exit(App().run(sys.argv))
