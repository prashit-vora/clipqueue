"""Run only on an isolated X11 display: DISPLAY=:97 PYTHONPATH=. python3 tests/integration.py."""
import os
import subprocess
import tempfile
import time
from pathlib import Path
import gi
gi.require_version('Gtk','3.0')
gi.require_version('Gdk','3.0')
from gi.repository import Gtk, Gdk, GdkPixbuf, GLib
from Xlib import X, XK, display
from Xlib.ext import xtest
from clipqueue import App
from saved_screenshots import ScreenshotWatcher

assert os.environ.get('DISPLAY') != ':0', 'Do not test on the user desktop'

def spin(seconds=.25):
    until = time.monotonic()+seconds
    while time.monotonic()<until:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        time.sleep(.003)

def copied(data, mime='UTF8_STRING'):
    process = subprocess.Popen(['xclip','-selection','clipboard','-t',mime],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    process.communicate(data,timeout=3)
    spin()

app = App()
app.set_application_id('local.clipqueue.Integration')
app.register(None)
app.activate()
spin()
assert app.window is not None
app.mode.set_active(True)
spin()
copied(b'first')
copied(b'second')
copied('third 🦋'.encode())
assert [c.data for c in app.queue.items] == ['first','second','third 🦋'], [c.label for c in app.queue.items]

sink = Gtk.Window(title='Test paste destination')
entry = Gtk.Entry()
sink.add(entry)
sink.show_all()
entry.grab_focus()
spin()
x = display.Display()
x.create_resource_object('window',sink.get_window().get_xid()).set_input_focus(X.RevertToParent,X.CurrentTime)
x.sync()

def ctrl_v(shift=False):
    names = ['Control_L']+(['Shift_L'] if shift else [])+['v']
    for name in names:
        xtest.fake_input(x,X.KeyPress,x.keysym_to_keycode(XK.string_to_keysym(name)))
    for name in reversed(names):
        xtest.fake_input(x,X.KeyRelease,x.keysym_to_keycode(XK.string_to_keysym(name)))
    x.flush()
    spin(.4)

for expected in ['first','firstsecond','firstsecondthird 🦋']:
    ctrl_v()
    assert entry.get_text() == expected, (entry.get_text(),expected)
assert len(app.queue.items)==0, 'Own clipboard writes must not be recaptured'
ctrl_v()
assert entry.get_text()=='firstsecondthird 🦋third 🦋', 'Empty queue must preserve ordinary paste'
app.undo()
assert app.queue.peek().data=='third 🦋'
app.clear()

# Same text copied twice should represent two distinct copy operations.
copied(b'repeated')
copied(b'repeated')
assert len(app.queue.items)==2
app.clear()

# Paused mode keeps the queue intact and forwards a normal Ctrl+V.
copied(b'queued')
app.mode.set_active(False)
copied(b'ordinary')
entry.set_text('')
ctrl_v()
assert entry.get_text()=='ordinary'
assert app.queue.peek().data=='queued'
app.mode.set_active(True)
app.clear()

image=GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB,True,8,96,64)
image.fill(0x51b987ff)
_,png=image.save_to_bufferv('png',[],[])
copied(png,'image/png')
copied(b'after image')
assert [c.kind for c in app.queue.items]==['image','text']
received=[]
def handle_key(widget,event):
    if event.keyval == Gdk.KEY_v and event.state & Gdk.ModifierType.CONTROL_MASK:
        Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD).request_image(lambda cb,pix,_: received.append(pix),None)
        return True
    return False
handler=entry.connect('key-press-event',handle_key)
ctrl_v()
assert received and received[0] is not None
assert (received[0].get_width(),received[0].get_height())==(96,64)
assert received[0].get_pixels()==image.get_pixels()
entry.disconnect(handler)
entry.set_text('')
ctrl_v()
assert entry.get_text()=='after image'
assert not app.queue.items

# New saved screenshots only; existing files and paused writes are excluded.
with tempfile.TemporaryDirectory() as folder:
    image.savev(str(Path(folder)/'Screenshot old.png'),'png',[],[])
    saved=[]
    watcher=ScreenshotWatcher(saved.append, print,[folder])
    watcher.set_enabled(True)
    spin()
    assert not saved
    image.savev(str(Path(folder)/'Screenshot new.png'),'png',[],[])
    spin(.65)
    assert len(saved)==1
    watcher.set_enabled(False)
    image.savev(str(Path(folder)/'Screenshot paused.png'),'png',[],[])
    spin(.4)
    assert len(saved)==1
    watcher.close()

# Exercise real region selection and cancellation on the isolated screen.
from screenshot import RegionPicker
regions=[]
picker=RegionPicker(regions.append)
spin()
xtest.fake_input(x,X.MotionNotify,x=200,y=200)
xtest.fake_input(x,X.ButtonPress,1)
xtest.fake_input(x,X.MotionNotify,x=360,y=290)
xtest.fake_input(x,X.ButtonRelease,1)
x.flush()
spin()
assert len(regions)==1 and regions[0] is not None
assert (regions[0].get_width(),regions[0].get_height())==(160,90)
picker=RegionPicker(regions.append)
spin()
xtest.fake_input(x,X.KeyPress,x.keysym_to_keycode(XK.string_to_keysym('Escape')))
xtest.fake_input(x,X.KeyRelease,x.keysym_to_keycode(XK.string_to_keysym('Escape')))
x.flush()
spin()
assert len(regions)==2 and regions[1] is None

# Screenshot a populated app for visual review.
app.receive(app.desktop.image_clip(image))
copied(b'A useful idea to keep for later.')
app.window.present()
spin()
if os.environ.get('CLIPQUEUE_PREVIEW'):
    window=app.window.get_window()
    shot=Gdk.pixbuf_get_from_window(window,0,0,window.get_width(),window.get_height())
    shot.savev(os.environ['CLIPQUEUE_PREVIEW'],'png',[],[])
app.desktop.close()
app.watcher.close()
sink.destroy()
app.window.destroy()
x.close()
print('PASS: FIFO text, unicode, duplicate copies, normal Ctrl+V, pause, empty queue, undo, images, no self-capture, saved screenshots, region capture and cancel.')
