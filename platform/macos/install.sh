#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source="$here/ClipQueue.app"
if [ ! -d "$source" ]; then source="$here/../../build/ClipQueue.app"; fi
if [ ! -d "$source" ]; then echo 'Build first, or extract the complete macOS download.' >&2; exit 1; fi
app="$HOME/Applications/ClipQueue.app"
agent="$HOME/Library/LaunchAgents/local.clipqueue.daemon.plist"
uid=$(id -u)
launchctl bootout "gui/$uid/local.clipqueue.daemon" 2>/dev/null || true
if [ -x "$app/Contents/MacOS/clipqueue" ]; then "$app/Contents/MacOS/clipqueue" stop 2>/dev/null || true; fi
mkdir -p "$HOME/Applications" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs/ClipQueue" "$HOME/.local/bin"
# ditto preserves the app signature; replacing does not require administrator access.
ditto "$source" "$app"
ln -sf "$app/Contents/MacOS/clipqueue" "$HOME/.local/bin/clipqueue"
/usr/libexec/PlistBuddy -c Clear "$agent" >/dev/null 2>&1 || true
/usr/libexec/PlistBuddy -c 'Add :Label string local.clipqueue.daemon' "$agent"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments array' "$agent"
/usr/libexec/PlistBuddy -c "Add :ProgramArguments:0 string $app/Contents/MacOS/clipqueue" "$agent"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments:1 string --daemon' "$agent"
/usr/libexec/PlistBuddy -c 'Add :RunAtLoad bool true' "$agent"
/usr/libexec/PlistBuddy -c "Add :StandardErrorPath string $HOME/Library/Logs/ClipQueue/daemon.log" "$agent"
launchctl bootstrap "gui/$uid" "$agent"
printf '%s\n' 'Installed. Enable ClipQueue in System Settings > Privacy & Security > Accessibility.' \
    "Then run: launchctl kickstart gui/$uid/local.clipqueue.daemon" \
    'Command+Option+Q toggles queue mode; Command+Option+Delete clears it. Command+V pastes the next item.'
