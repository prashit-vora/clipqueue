# ClipQueue

Copy a few things. Paste them in the same order.

## Use it

Open **ClipQueue** from your applications menu, or double-click `Launch.sh` and choose Run.

1. Turn on the switch at the top right.
2. Use your existing **Ctrl+C** and screenshot shortcuts several times.
3. Click the destination and press your usual **Ctrl+V** once per item.

Clipboard-manager handoffs are ignored when they contain the image or text already captured, so a screenshot is added once. Separate copies of identical content are still kept as separate items.

Text and images share one queue. The oldest item is pasted first. When the queue is empty, Ctrl+V works normally. Turn the switch off to return to ordinary clipboard use while keeping queued items.

**No desktop shortcut settings are changed and no new shortcuts are added.** ClipQueue temporarily handles Ctrl+V while it is running so it can supply the next item, then forwards your original keystroke to the destination. Ctrl+Shift+V and other shortcuts retain their existing behavior and do not advance the queue. Right-click Paste does not advance it either.

Your existing screenshot-to-clipboard shortcuts work. The app also picks up newly created files named `Screenshot…png/jpg/jpeg` in Pictures, Pictures/Screenshots (if it exists when the app starts), and the screenshot tool's configured save folders. It does not import old screenshots. Screenshots with custom names or saved elsewhere can be copied to the clipboard. The **Capture area** button is an optional way to capture directly into the queue without saving a file; drag to select, Escape to cancel.

Close the window to leave ClipQueue running in the tray, or minimized if no tray is available. Click the tray icon or reopen the launcher to see it. **Quit** exits and forgets the queue. It does not start automatically at login.

## Controls

- **Restore last paste:** puts the last sent item back at the front if you pasted in the wrong place or the destination rejected it. It does not undo changes in the destination.
- **×:** removes one queued item.
- **Clear:** forgets all queued items and the restore item. The current system clipboard remains unchanged.
- **Queue switch:** turns collection and FIFO pasting on or off.

Images need a destination that accepts pasted images. ClipQueue cannot know whether another application accepted a paste; it advances when it forwards Ctrl+V. Text is collected as plain text, so source formatting is not preserved. Files and arbitrary clipboard formats are not supported.

## Privacy and limits

ClipQueue stores its queue only in memory, with no network requests or history files. Collecting is off when you first launch it. While it is on, anything copied as text or an image can enter the queue; pause it when you don't want copies retained. Screenshot files created by your existing screenshot tool still remain where that tool saved them. The system clipboard and any other clipboard manager are separate from ClipQueue's memory.

The queue holds up to 200 items or 256 MB of image/text data. If full, collection pauses without dropping the existing queue. Paste or remove items, then turn collection back on. Allow each copy to arrive in the queue before replacing the clipboard again; content overwritten before the desktop transfers it cannot be recovered.

## Current status

The clipboard-manager duplicate-capture fix passes its regression tests. The live desktop workflow has also been confirmed working after restart.

## Supported computer

This release supports **Linux X11**, tested on **Linux Mint 22.1 / Cinnamon**. Wayland, Windows and macOS are not supported yet. `queue_model.py` is independent of the desktop, so the queue can be reused when native backends are added for those systems. Supporting them also requires platform-specific clipboard, permission, screenshot and shortcut integration, and testing on each OS.

Uses Python 3, GTK 3, PyGObject, python-xlib and Cairo. On Debian/Ubuntu/Mint X11, install the relevant packages with:

```sh
sudo apt install python3 python3-gi python3-gi-cairo python3-xlib gir1.2-gtk-3.0
```

Run `python3 install.py` from this folder to add or refresh the applications-menu launcher. Keep the folder in place afterwards. To uninstall the launcher, remove `~/.local/share/applications/clipqueue.desktop`; quit ClipQueue and delete its folder if desired.

## Validation

The automated suite checks FIFO ordering, mixed text/images, duplicate copies, queue limits, restore, remove and clear. The isolated desktop integration test exercises actual Ctrl+V input, Unicode, paused mode, empty-queue fallback, exact image pixel transfer, prevention of self-capture, new saved screenshots, area selection and cancellation. A separate regression test runs Mint’s real clipboard service to verify screenshot handoffs, intentional repeated copies, and handoffs after a queue paste.

```sh
python3 -m unittest discover -s tests -v
# Integration test requires xclip and an isolated X11 display, such as Xvfb:
DISPLAY=:97 XDG_SESSION_TYPE=x11 PYTHONPATH=. python3 tests/integration.py
DISPLAY=:97 PYTHONPATH=. dbus-run-session -- python3 tests/clipboard_handoff.py
```

The backend uses GTK's [clipboard API](https://docs.gtk.org/gtk3/class.Clipboard.html) and X11's [keyboard event replay](https://www.x.org/releases/X11R7.6/doc/libX11/specs/libX11/libX11.html).
