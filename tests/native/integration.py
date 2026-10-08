"""Development-only tests; GTK/Python are NOT application dependencies.
Run with DISPLAY=:97 on an isolated Xvfb server.
"""
import json, os, subprocess, time, tempfile, struct, zlib
from pathlib import Path
import gi
gi.require_version('Gtk','3.0')
gi.require_version('Gdk','3.0')
from gi.repository import Gtk,Gdk,GdkPixbuf
from Xlib import X,XK,display
from Xlib.ext import xtest

assert os.environ.get('DISPLAY') not in (None,':0'), 'Use an isolated X server'
ROOT=Path(__file__).resolve().parents[2]
BIN=str(ROOT/'build/clipqueue')

def spin(seconds=.15):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  while Gtk.events_pending(): Gtk.main_iteration_do(False)
  time.sleep(.002)

def ctl(cmd):
 return json.loads(subprocess.check_output([BIN,cmd],text=True))

def copy(data,mime='UTF8_STRING'):
 p=subprocess.Popen(['xclip','-selection','clipboard','-t',mime],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 p.communicate(data,timeout=5)
 spin(.25)

def paste_read(target='UTF8_STRING'):
 hotkey(['Control_L','v'])
 return subprocess.check_output(['xclip','-selection','clipboard','-t',target,'-o'],timeout=5)

def hotkey(names):
 for name in names: xtest.fake_input(x,X.KeyPress,x.keysym_to_keycode(XK.string_to_keysym(name)))
 for name in reversed(names): xtest.fake_input(x,X.KeyRelease,x.keysym_to_keycode(XK.string_to_keysym(name)))
 x.flush();spin(.3)

with tempfile.TemporaryDirectory() as folder:
 env=dict(os.environ,HOME=folder,XDG_CONFIG_HOME=folder+'/config')
 Path(folder,'Pictures').mkdir()
 daemon=subprocess.Popen([BIN,'--daemon'],env=env)
 sink=None
 try:
  spin(.25)
  assert daemon.poll() is None
  assert not ctl('status')['enabled']
  assert subprocess.run([BIN,'--daemon'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode!=0,'single instance'
  x=display.Display()
  sink=Gtk.Window(title='Paste target')
  entry=Gtk.Entry();sink.add(entry);sink.show_all();entry.grab_focus();spin()
  x.create_resource_object('window',sink.get_window().get_xid()).set_input_focus(X.RevertToParent,X.CurrentTime);x.sync()
  hotkey(['Control_L','Alt_L','q'])
  assert ctl('status')['enabled'],'toggle hotkey on'
  for s in ['first','second','third 🦋']: copy(s.encode())
  assert ctl('status')['queued']==3
  for expected in ['first','firstsecond','firstsecondthird 🦋']:
   hotkey(['Control_L','v']);assert entry.get_text()==expected,(entry.get_text(),expected)
  assert ctl('status')['queued']==0,'no own-write recapture'
  hotkey(['Control_L','v']);assert entry.get_text().endswith('third 🦋third 🦋'),'empty queue passthrough'
  assert ctl('undo')['queued']==1
  ctl('clear')
  copy(b'queued')
  hotkey(['Control_L','Alt_L','q']);assert not ctl('status')['enabled']
  copy(b'ordinary');entry.set_text('');hotkey(['Control_L','v']);assert entry.get_text()=='ordinary'
  assert ctl('status')['queued']==1
  ctl('on');hotkey(['Control_L','Alt_L','BackSpace']);assert ctl('status')['queued']==0
  # Consecutive duplicate text, alias targets, nonconsecutive repeats and reset.
  for value in [b'A',b'A',b'A',b'B',b'A',b'A']: copy(value)
  assert ctl('status')['queued']==3,ctl('status')
  ctl('clear');copy(b'alias');copy(b'alias','text/plain');assert ctl('status')['queued']==1
  ctl('clear');copy(b'alias');assert ctl('status')['queued']==1
  ctl('clear');copy(b'caf\xe9','STRING');assert paste_read()=='café'.encode()
  ctl('clear')
  # Real incremental clipboard transfer, in both directions.
  large=('Large data 🦋 '*60000).encode()
  copy(large);assert ctl('status')['queued']==1,ctl('status')
  result=paste_read();assert result==large,(len(result),len(large))
  assert ctl('status')['queued']==0
  # Image plus text FIFO and exact PNG bytes retained for output.
  image=GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB,True,8,96,64);image.fill(0x51b987ff)
  _,png=image.save_to_bufferv('png',[],[])
  copy(png,'image/png')
  metadata=b'Comment\0same pixels, another PNG encoding'
  tag=b'tEXt'+metadata
  variant=png[:33]+struct.pack('>I',len(metadata))+tag+struct.pack('>I',zlib.crc32(tag))+png[33:]
  copy(variant,'image/png');copy(png,'image/png')
  assert ctl('status')['queued']==1,ctl('status')
  copy(b'after image')
  assert ctl('status')['queued']==2
  assert paste_read('image/png')==png
  assert paste_read()==b'after image'
  # The copied text breaks consecutiveness, so this image should be accepted.
  # Saved PNG with existing shortcut naming enters via inotify.
  image.savev(str(Path(folder,'Pictures','Screenshot test.png')),'png',[],[])
  spin(.3);assert ctl('status')['queued']==1,ctl('status')
  ctl('clear');assert ctl('status')['queued']==0
  ctl('stop');daemon.wait(timeout=3)
  assert daemon.returncode==0
  print('PASS: native FIFO, normal Ctrl+V, Unicode, large INCR transfers, PNG pixel dedup, text dedup, pause, clear, undo, toggle/clear hotkeys, saved screenshot, single instance, graceful stop')
 finally:
  if daemon.poll() is None: daemon.terminate();daemon.wait(timeout=3)
  if sink: sink.destroy()
