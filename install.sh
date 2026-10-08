#!/bin/sh
set -eu
prefix=${1:-"$HOME/.local"}
config_root=${XDG_CONFIG_HOME:-"$HOME/.config"}
data_root=${XDG_DATA_HOME:-"$HOME/.local/share"}
case "$prefix" in /*) ;; *) echo 'Install prefix must be an absolute path.' >&2; exit 1;; esac
install -Dm755 build/clipqueue "$prefix/bin/clipqueue"
mkdir -p "$config_root/clipqueue" "$config_root/autostart" "$data_root/applications"
if [ ! -f "$config_root/clipqueue/config" ]; then
    cat > "$config_root/clipqueue/config" <<'CONFIG'
# X11 key names. Restart ClipQueue after changing these.
toggle_key=Control+Alt+q
clear_key=Control+Alt+BackSpace
# Hide temporary toggle/clear status pop-ups:
quiet=false
# Optional extra directory for newly saved PNG screenshots:
# screenshot_dir=/absolute/path
CONFIG
fi
# Desktop entry escaping for paths with spaces, quotes, backslashes, or percent.
escaped=$(printf '%s' "$prefix/bin/clipqueue" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g; s/%/%%/g')
cat > "$config_root/autostart/clipqueue.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=ClipQueue background service
Comment=Collect and paste text and images in order
Exec="$escaped" start
Terminal=false
X-GNOME-Autostart-enabled=true
DESKTOP
cp "$config_root/autostart/clipqueue.desktop" "$data_root/applications/clipqueue.desktop"
printf 'Installed %s/bin/clipqueue\nStarts in background at login, with queue mode off.\n' "$prefix"
