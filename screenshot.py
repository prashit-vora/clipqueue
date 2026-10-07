import gi
gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
from gi.repository import Gtk, Gdk


class RegionPicker(Gtk.Window):
    def __init__(self, finished):
        super().__init__(type=Gtk.WindowType.POPUP)
        self.finished = finished
        self.start = self.end = None
        self.done = False
        root = Gdk.get_default_root_window()
        self.width, self.height = root.get_width(), root.get_height()
        self.background = Gdk.pixbuf_get_from_window(root, 0, 0, self.width, self.height)
        if self.background is None:
            raise RuntimeError('Could not capture this X11 screen.')
        self.scale = self.background.get_width() / self.width
        self.set_app_paintable(True)
        self.set_keep_above(True)
        self.move(0, 0)
        self.set_default_size(self.width, self.height)
        self.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
                        Gdk.EventMask.POINTER_MOTION_MASK | Gdk.EventMask.KEY_PRESS_MASK)
        self.connect('draw', self.draw)
        self.connect('button-press-event', self.press)
        self.connect('motion-notify-event', self.motion)
        self.connect('button-release-event', self.release)
        self.connect('key-press-event', self.key)
        self.show_all()
        self.get_window().set_cursor(Gdk.Cursor.new_from_name(Gdk.Display.get_default(), 'crosshair'))
        self.seat = Gdk.Display.get_default().get_default_seat()
        result = self.seat.grab(self.get_window(), Gdk.SeatCapabilities.ALL, False, None, None, None, None)
        if result != Gdk.GrabStatus.SUCCESS:
            self.destroy()
            raise RuntimeError('Could not start capture. Close any open menu and try again.')

    def bounds(self):
        x1, y1 = self.start
        x2, y2 = self.end
        return int(min(x1,x2)), int(min(y1,y2)), int(abs(x2-x1)), int(abs(y2-y1))

    def draw(self, widget, cr):
        cr.save()
        cr.scale(1 / self.scale, 1 / self.scale)
        Gdk.cairo_set_source_pixbuf(cr, self.background, 0, 0)
        cr.paint()
        cr.restore()
        cr.set_source_rgba(.03,.04,.07,.55)
        cr.paint()
        if self.start and self.end:
            x,y,w,h = self.bounds()
            cr.save()
            cr.rectangle(x,y,w,h)
            cr.clip()
            cr.scale(1 / self.scale, 1 / self.scale)
            Gdk.cairo_set_source_pixbuf(cr, self.background, 0, 0)
            cr.paint()
            cr.restore()
            cr.set_source_rgb(.52,.84,.67)
            cr.set_line_width(2)
            cr.rectangle(x+.5,y+.5,w,h)
            cr.stroke()
        cr.set_source_rgb(1,1,1)
        cr.select_font_face('Sans',0,1)
        cr.set_font_size(19)
        cr.move_to(28,38)
        cr.show_text('Drag an area to add it to your queue · Esc to cancel')
        return True

    def press(self, widget, event):
        if event.button == 1:
            self.start = self.end = (event.x,event.y)
        elif event.button == 3:
            self.finish(None)
        return True

    def motion(self, widget, event):
        if self.start:
            self.end = (max(0,min(self.width,event.x)),max(0,min(self.height,event.y)))
            self.queue_draw()
        return True

    def release(self, widget, event):
        if event.button != 1 or not self.start:
            return True
        self.motion(widget,event)
        x,y,w,h = self.bounds()
        image = None
        if w > 3 and h > 3:
            s = self.scale
            image = self.background.new_subpixbuf(int(x*s),int(y*s),int(w*s),int(h*s)).copy()
        self.finish(image)
        return True

    def key(self, widget, event):
        if event.keyval == Gdk.KEY_Escape:
            self.finish(None)
        return True

    def finish(self, image):
        if self.done:
            return
        self.done = True
        self.seat.ungrab()
        self.destroy()
        self.finished(image)
