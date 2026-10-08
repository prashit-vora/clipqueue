#!/bin/sh
set -eu
launchctl bootout "gui/$(id -u)/local.clipqueue.daemon" 2>/dev/null || true
if [ -x "$HOME/.local/bin/clipqueue" ]; then "$HOME/.local/bin/clipqueue" stop 2>/dev/null || true; fi
rm -f "$HOME/Library/LaunchAgents/local.clipqueue.daemon.plist" "$HOME/.local/bin/clipqueue"
rm -rf "$HOME/Applications/ClipQueue.app"
echo 'ClipQueue removed. Its empty logs and macOS permission entry can be removed separately.'
