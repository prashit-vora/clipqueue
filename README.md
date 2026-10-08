# ClipQueue

A small **C background daemon for Linux X11**. Collect text and screenshots, then paste them in first-in, first-out order using ordinary **Ctrl+V**. No application window, Python runtime, GTK, Electron, package downloads, or network access at runtime.

## Controls

| Action | Shortcut |
|---|---|
| Toggle queue mode | **Ctrl+Alt+Q** |
| Clear the queue | **Ctrl+Alt+Backspace** |
| Paste the oldest queued item | **Ctrl+V** |
| Copy text / take screenshots | Your existing shortcuts |

A brief status pop-up confirms toggle/clear actions; it never takes keyboard focus. Queue mode starts **off**. When off, or when the queue is empty, Ctrl+V works normally. Ctrl+Shift+V, right-click Paste, and middle-click retain their normal behavior and do not advance the queue.

## Build and install

```sh
make
make install
~/.local/bin/clipqueue start
```

The compiler and X11 development headers are needed **only to build**. On Debian/Ubuntu/Mint: `build-essential libx11-dev libxfixes-dev zlib1g-dev`. The running program uses the system C, X11, XFixes, and zlib libraries already supplied by an X11 desktop. It does not mean literally zero shared libraries.

The installer adds `~/.local/bin/clipqueue`, a configuration file, and a desktop-login autostart entry. The process runs in the background, initially paused, after each graphical login. It needs no root access. The applications-menu launcher also starts the background process; it does not open a window.

```sh
clipqueue status  # Mode, item count, memory and last action; never clipboard contents
clipqueue on
clipqueue off
clipqueue toggle
clipqueue clear
clipqueue undo    # Restore the last pasted item to the front
clipqueue stop   # Gracefully stop; clears the in-memory queue
clipqueue --daemon  # Foreground mode for debugging or a service supervisor
```

If `~/.local/bin` is not in your PATH, use the full executable path.

## Configuration

Edit `~/.config/clipqueue/config` (or `$XDG_CONFIG_HOME/clipqueue/config`), then stop/start the daemon:

```ini
toggle_key=Control+Alt+q
clear_key=Control+Alt+BackSpace
quiet=false
# screenshot_dir=/absolute/path/to/screenshots
```

A conflicting shortcut causes startup to fail instead of changing another program's binding. Copy, paste and screenshot settings in the desktop are never rewritten. `quiet=true` disables temporary status pop-ups.

## Consecutive duplicate filtering

SHA-256 fingerprints filter repeated copies **only when consecutive**:

- `A, A, A` adds A once; `A, B, A` keeps all three.
- Text is compared as exact UTF-8 content. Case, spaces and newlines matter. Latin-1 clipboard text is converted to UTF-8 first.
- Static PNGs up to 16 megapixels are decoded to canonical RGBA16 samples; dimensions and pixels are hashed. Different compression, row filters, RGB/RGBA encodings or descriptive metadata do not create duplicates. Color-management metadata is not part of this pixel comparison. Original encoded bytes are preserved for paste.
- JPEGs, animated PNGs, oversized PNGs and PNGs the bounded decoder cannot normalize use an exact encoded-byte hash. Recopying the same encoded image is filtered; differently encoded JPEGs are not guaranteed to match.
- Clearing the queue, restoring an item, or changing queue mode resets consecutive-copy tracking, allowing an intentional recopy. Pasting alone does not reset it.
- Hashing is local, in memory, using SHA-256 implemented in C. PNG decompression uses the system zlib library, not a separate image framework.

## Clipboard behavior

- UTF-8 text, PNG images and JPEG images share one FIFO queue. Text is plain text, without rich formatting.
- Mint clipboard-manager handoffs are ignored when they republish content already captured.
- Large clipboard transfers use X11's incremental transfer protocol in both directions.
- Screenshot-to-clipboard keys work unchanged. New PNG files named `Screenshot…` saved under `~/Pictures`, `~/Pictures/Screenshots`, or the configured extra screenshot folder also enter the queue. Watched folders must exist at startup. Existing files are not imported.
- Pasting advances the queue when Ctrl+V is forwarded. Another application may reject the content (for example, images in a plain text editor); `clipqueue undo` restores the last item for another attempt. It does not undo edits in that application.
- Clearing the queue leaves the system clipboard alone. On graceful stop, the daemon asks the desktop clipboard manager to retain the last clipboard item if one is available. Abrupt termination can lose content currently owned by the process.
- Capacity: 200 items, 256 MiB of queued payloads, and 64 MiB per captured item. Current/last-paste and active transfer references can retain additional payloads. At capacity, collection pauses rather than dropping existing entries.
- There is no clipboard history on disk. Stopping or logging out forgets the queue. Screenshot files saved by the desktop remain in their normal folder.

## Platform scope

This release runs on **Linux X11**, tested on Linux Mint 22.1/Cinnamon. Wayland, Windows and macOS are future work. A background process still needs each OS's native clipboard and keyboard integration; changing language alone does not make those interfaces portable.

## Tests

Development-only integration tests use Python, GTK, python-xlib and xclip to exercise real X11 selection transfers. None of these are runtime dependencies of the daemon. Run them on an **isolated** Xvfb server, never your working desktop:

```sh
make test
DISPLAY=:97 XDG_SESSION_TYPE=x11 python3 tests/native/integration.py
DISPLAY=:97 XDG_SESSION_TYPE=x11 dbus-run-session -- python3 tests/native/handoff.py
```

They cover standard SHA-256 vectors, all PNG row filters, pixel identity, transparency, indexed/grayscale/16-bit and interlaced PNGs, malformed inputs, consecutive-copy filtering, FIFO text/images, Unicode, large transfers, clipboard-manager handoffs, normal paste passthrough, queue controls, restore, saved screenshots, startup conflicts and clipboard preservation on graceful shutdown. The handoff regression requires Mint's `csd-clipboard`.

## Uninstall

Stop the daemon and remove `~/.local/bin/clipqueue`, `~/.config/autostart/clipqueue.desktop`, and `~/.local/share/applications/clipqueue.desktop`. Remove `~/.config/clipqueue` if you also want to discard its shortcut preferences. Adjust paths if you installed with custom XDG directories or a custom prefix.
