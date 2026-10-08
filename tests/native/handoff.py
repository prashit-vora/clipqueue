"""Run under dbus-run-session on an isolated X11 display (Mint csd-clipboard)."""
import os,subprocess,time,json,tempfile
from Xlib import X,XK,display
from Xlib.ext import xtest
from pathlib import Path
assert os.environ.get('DISPLAY') not in (None,':0')
BIN=str(Path(__file__).resolve().parents[2]/'build/clipqueue')
PRODUCER='''
import sys,gi
gi.require_version('Gtk','3.0')
from gi.repository import Gtk,Gdk,GdkPixbuf,GLib
cb=Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
if sys.argv[1]=='image':
 image=GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB,True,8,80,60)
 image.fill(0x445566ff)
 cb.set_image(image)
else: cb.set_text(sys.argv[1],-1)
def finish():
 cb.store();Gtk.main_quit();return False
GLib.timeout_add(100,finish)
Gtk.main()
'''
def ctl(cmd): return json.loads(subprocess.check_output([BIN,cmd],text=True))
with tempfile.TemporaryDirectory() as folder:
 env=dict(os.environ,HOME=folder,XDG_CONFIG_HOME=folder)
 manager=subprocess.Popen(['/usr/bin/csd-clipboard'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 time.sleep(.3)
 daemon=subprocess.Popen([BIN,'--daemon'],env=env)
 try:
  time.sleep(.3);ctl('on')
  for item,count in [('image',1),('image',1),('text',2),('text',2),('image',3)]:
   subprocess.run(['/usr/bin/python3','-c',PRODUCER,item],check=True,timeout=5)
   time.sleep(.2)
   assert ctl('status')['queued']==count,(item,ctl('status'))
  # Recopying an earlier pasted image is a new copy if text intervened.
  ctl('clear')
  for item in ['image','text']:
   subprocess.run(['/usr/bin/python3','-c',PRODUCER,item],check=True,timeout=5)
   time.sleep(.15)
  x=display.Display()
  for name in ['Control_L','v']: xtest.fake_input(x,X.KeyPress,x.keysym_to_keycode(XK.string_to_keysym(name)))
  for name in ['v','Control_L']: xtest.fake_input(x,X.KeyRelease,x.keysym_to_keycode(XK.string_to_keysym(name)))
  x.flush();time.sleep(.25)
  assert ctl('status')['queued']==1
  subprocess.run(['/usr/bin/python3','-c',PRODUCER.replace('timeout_add(100','timeout_add(1'),'image'],check=True,timeout=5)
  time.sleep(.3);assert ctl('status')['queued']==2,ctl('status')
  # Serve a paste then retain it through the real clipboard manager on stop.
  for name in ['Control_L','v']: xtest.fake_input(x,X.KeyPress,x.keysym_to_keycode(XK.string_to_keysym(name)))
  for name in ['v','Control_L']: xtest.fake_input(x,X.KeyRelease,x.keysym_to_keycode(XK.string_to_keysym(name)))
  x.flush();time.sleep(.25)
  before=subprocess.check_output(['xclip','-selection','clipboard','-o'],timeout=3)
  ctl('stop');daemon.wait(timeout=3)
  after=subprocess.check_output(['xclip','-selection','clipboard','-o'],timeout=3)
  assert before==after==b'text', 'graceful stop preserves clipboard'
  x.close()
  print('PASS: Mint handoffs and repeated copies are deduplicated; nonconsecutive copies remain')
 finally:
  if daemon.poll() is None: daemon.terminate();daemon.wait(timeout=3)
  manager.terminate();manager.wait(timeout=3)
