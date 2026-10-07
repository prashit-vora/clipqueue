"""Watch new screenshots saved by the existing desktop screenshot shortcuts."""
from pathlib import Path
import re
import gi
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import Gio, GLib, GdkPixbuf


class ScreenshotWatcher:
    def __init__(self, received, status, directories=None):
        self.received, self.status = received, status
        self.enabled = False
        self.epoch = 0
        self.pending = set()
        self.monitors = []
        if directories is None:
            pictures = Path(GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_PICTURES) or Path.home() / 'Pictures')
            directories = {pictures, pictures / 'Screenshots'}
            try:
                settings = Gio.Settings.new('org.gnome.gnome-screenshot')
                for key in ['auto-save-directory', 'last-save-directory']:
                    location = settings.get_string(key)
                    if location:
                        path = Gio.File.new_for_uri(location).get_path() if location.startswith('file:') else location
                        if path:
                            directories.add(Path(path))
            except Exception:
                pass
        self.directories = [Path(path) for path in directories]
        self.known = set()
        self.scan_baseline()
        for folder in self.directories:
            if folder.is_dir():
                monitor = Gio.File.new_for_path(str(folder)).monitor_directory(Gio.FileMonitorFlags.WATCH_MOVES, None)
                monitor.connect('changed', self.changed)
                self.monitors.append(monitor)

    def scan_baseline(self):
        self.known = {str(path) for folder in self.directories if folder.is_dir() for path in folder.iterdir()}

    def set_enabled(self, enabled):
        self.enabled = enabled
        self.epoch += 1
        self.pending.clear()
        self.scan_baseline()

    def changed(self, monitor, file, other_file, event):
        path = file.get_path()
        if not path or not self.enabled or path in self.known or path in self.pending:
            return
        name = Path(path).name
        if not re.match(r'(?i)^screenshot(?:[ _-]|$).*\.(png|jpe?g)$', name):
            return
        self.pending.add(path)
        epoch = self.epoch
        attempts, last_size = 0, -1
        def load():
            nonlocal attempts, last_size
            if epoch != self.epoch or not self.enabled:
                return False
            attempts += 1
            try:
                size = Path(path).stat().st_size
                if size != last_size or size == 0:
                    last_size = size
                    if attempts < 20:
                        return True
                image = GdkPixbuf.Pixbuf.new_from_file(path)
                self.known.add(path)
                self.pending.discard(path)
                self.received(image)
                return False
            except Exception:
                if attempts < 20:
                    return True
                self.pending.discard(path)
                self.status('A saved screenshot could not be read. Copy it to the clipboard to add it.')
                return False
        GLib.timeout_add(150, load)

    def close(self):
        self.enabled = False
        for monitor in self.monitors:
            monitor.cancel()
