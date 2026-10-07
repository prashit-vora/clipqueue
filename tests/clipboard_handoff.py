"""Regression for Mint's clipboard handoff.

Run on an isolated X server and bus:
DISPLAY=:97 PYTHONPATH=. dbus-run-session -- python3 tests/clipboard_handoff.py
"""
import os
import subprocess
import time
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk
from desktop import Desktop

assert os.environ.get('DISPLAY') not in (None, ':0'), 'Use an isolated test desktop'


def spin(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        time.sleep(.004)


PRODUCER = '''
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk, GdkPixbuf, GLib
cb = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
if sys.argv[1] == 'image':
    image = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, 80, 60)
    image.fill(0x445566ff)
    cb.set_image(image)
else:
    cb.set_text('same text', -1)
def finish():
    cb.store()
    Gtk.main_quit()
    return False
GLib.timeout_add(350, finish)
Gtk.main()
'''

manager = subprocess.Popen(['/usr/bin/csd-clipboard'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
desktop = None
try:
    spin(.6)
    assert manager.poll() is None, 'Test clipboard manager did not start'
    clips = []
    desktop = Desktop(clips.append, print)
    desktop.set_collecting(True)
    for kind, count in [('image', 1), ('image', 2), ('text', 3), ('text', 4)]:
        producer = subprocess.Popen(['/usr/bin/python3', '-c', PRODUCER, kind])
        try:
            spin(1.1)
            producer.wait(timeout=2)
        finally:
            if producer.poll() is None:
                producer.kill()
                producer.wait()
        assert len(clips) == count, f'{kind}: expected {count}, got {len(clips)}'
        assert desktop.owner() == desktop.x.get_selection_owner(desktop.manager_atom).id
    assert [c.kind for c in clips] == ['image', 'image', 'text', 'text']
    # Republishing our own paste through the manager must not add it again.
    desktop.write(clips[0])
    desktop.clipboard.store()
    spin(.5)
    assert len(clips) == 4, 'Own paste was recaptured through the manager'
    print('PASS: one screenshot → one item; deliberate repeated images/text preserved; own paste handoff ignored.')
finally:
    if desktop:
        desktop.close()
    manager.terminate()
    manager.wait(timeout=2)
