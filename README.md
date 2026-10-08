# ClipQueue

A small **native C background clipboard queue for Linux X11, Windows and macOS**. Copy text or screenshots several times, then paste in first-in, first-out order using **Ctrl+V** (Windows/Linux) or **Command+V** (Mac). No permanent window, Electron, Python runtime, downloaded runtime packages, or network access.

## Controls

| Action | Windows / Linux | macOS |
|---|---|---|
| Toggle queue mode | Ctrl+Alt+Q | Command+Option+Q |
| Clear queue | Ctrl+Alt+Backspace | Command+Option+Delete |
| Paste next item | Ctrl+V | Command+V |
| Copy or screenshot | Existing shortcuts | Existing shortcuts |

Queue mode starts **off** at each launch. Turning it off keeps queued items for later. When off or empty, normal paste passes through. Other paste commands (menus, right-click, Ctrl+Shift+V) do not advance the queue. Shortcuts in OS settings are never rewritten. Linux shows a temporary status overlay, Windows plays a system sound, and every platform supports `clipqueue status`.

## Install

### Windows 10 / 11 (x64)

Extract the Windows build, then run `install.ps1` in PowerShell. If PowerShell's policy blocks local scripts, run the program directly instead of changing system policy:

```powershell
.\clipqueue.exe start
.\clipqueue.exe on
.\clipqueue.exe status
```

The installer copies the executable to `%LOCALAPPDATA%\ClipQueue` and creates a login startup shortcut. No administrator access or VC++ redistributable is required. Use `uninstall.ps1` to remove it.

To build from source, install Visual Studio Build Tools with **Desktop development with C++**, then run `platform/windows/build.ps1`. The C runtime is statically linked; clipboard, keyboard and image decoding use Windows system DLLs.

### macOS 11+ (Apple Silicon and Intel)

Extract the universal macOS archive and run `sh install.sh`. It installs a background app at `~/Applications/ClipQueue.app`, a command at `~/.local/bin/clipqueue`, and a per-user login LaunchAgent. No Dock icon or main window.

Enable **ClipQueue** in **System Settings → Privacy & Security → Accessibility**, then run the restart command printed by the installer. macOS may also request Input Monitoring. Keyboard controls require the OS permission; the program cannot grant it itself.

```sh
~/.local/bin/clipqueue status
~/.local/bin/clipqueue on
launchctl kickstart "gui/$(id -u)/local.clipqueue.daemon"
```

Builds are ad-hoc signed, **not Developer ID signed or notarized**. macOS may block a downloaded build. You can inspect/build the source locally, or use Apple's explicit Open Anyway approval for the downloaded app. Replacing an ad-hoc signed build can require granting Accessibility again. The installer does not disable Gatekeeper or other security settings.

To build, install Apple's Command Line Tools and run `platform/macos/build.sh`. Only the system ApplicationServices, CoreFoundation and ImageIO frameworks are linked. Run `sh uninstall.sh` from the extracted archive to uninstall.

### Linux X11

```sh
make
make install
~/.local/bin/clipqueue start
```

Uses system X11, XFixes and zlib. Full Linux installation, configuration and screenshot-folder support: [Linux guide](docs/linux.md). **Wayland is not supported.**

## Queue and duplicate filtering

- One FIFO for plain text and images. Rich text and copied file lists are not preserved.
- SHA-256 filters **consecutive** duplicate copies: `A A A` adds one item; `A B A` adds three.
- Text is normalized to UTF-8, then compared exactly. Spaces, case and newlines matter.
- Windows accepts Unicode text, PNG and DIB/DIBV5 screenshots. macOS accepts UTF-8/UTF-16 text, PNG, TIFF and JPEG. Native image decoders hash pixel data plus dimensions for images up to 16 megapixels, so repeated copies can match despite different file encoding or metadata. Original bytes are retained for paste. Unsupported image normalization falls back to hashing original bytes. Lossy encodings, color profiles and alpha rounding can produce different pixels.
- Linux uses its bounded PNG decoder; details are in the Linux guide.
- Pasting does not reset duplicate tracking. Clearing the queue or changing modes does.
- Capacity: 200 entries, 256 MiB of queued payloads, at most 64 MiB per item. New entries are rejected at capacity; existing entries are not silently evicted.
- Nothing is saved as clipboard history or sent over the network. Stop/log out forgets the queue. Clearing the queue leaves the system clipboard unchanged.

On Windows/Mac, use screenshot shortcuts that **copy to the clipboard**. For example, Win+Shift+S or Command+Control+Shift+4. Screenshot shortcuts that only save a file do not change the clipboard, so they cannot be captured by these ports. Your shortcuts are not changed. Linux retains its existing screenshot-folder watcher.

The queue advances when a paste key event is delivered, since arbitrary destination apps provide no confirmation that they accepted an image/text. Give the destination time to paste before pressing again; an application that rejects the content still consumes the entry. Windows cannot inject paste into an elevated administrator application from a normal process. Mac Secure Input can prevent global keyboard handling. The new ports need real-desktop compatibility testing beyond the automated clipboard tests.

## Commands

`start`, `status`, `on`, `off`, `toggle`, `clear`, `stop`, and `--daemon` are available on all platforms. `--daemon` runs directly for service supervisors. Linux additionally supports `undo` and configurable shortcuts. The Windows/Mac ports currently use fixed control shortcuts and have no undo command.

## Memory

The **2–3 MB idle RAM target is not a guaranteed cross-platform limit**. Native system libraries, keyboard services and image codecs contribute to process memory; screenshots require additional space. A single uncompressed 1920×1080 RGBA screenshot is about **7.9 MiB**, before queue/OS overhead. The existing Linux version measured about 2.5 MiB RSS with an empty queue.

CI publishes actual Windows working-set/private-byte and Mac resident-memory samples beside the builds. `--probe` runs an empty process for two seconds and reports memory **without trimming its working set**. The report states whether the keyboard hook was available. A Mac CI sample without Accessibility permission is only a clipboard/event-loop baseline, not the fully authorized daemon. These samples are not a promise about other machines, long sessions, or image-heavy use.

## Tests and builds

GitHub Actions compiles on Linux, Windows and Mac, tests the portable C queue, exercises the Windows/Mac native Unicode clipboard round trip, and produces installable Windows x64 and Mac universal artifacts. Linux also runs image identity tests; portable queue tests run under AddressSanitizer/UndefinedBehaviorSanitizer.

```sh
make test
cc -std=c11 -Isrc tests/portable/core_test.c src/core.c src/sha256.c -o build/core-test
build/core-test
```

The portable tests check FIFO order, consecutive duplicates across pastes and mode changes, image identity independent of encoded bytes, dimensions, capacity rejection/recovery and SHA-256. Native `--self-test` replaces the test machine's clipboard; run it in an isolated development session. macOS CI cannot grant user Accessibility permission, so it does not prove that global shortcuts work on an authorized desktop. Linux's fuller Xvfb integration suite is documented in the Linux guide.
