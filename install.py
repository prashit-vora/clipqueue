#!/usr/bin/env python3
"""Install only a user applications-menu entry; does not modify shortcuts."""
import os
from pathlib import Path

base = Path(__file__).resolve().parent
applications = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'applications'
applications.mkdir(parents=True, exist_ok=True)

def quote_exec(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('`','\\`').replace('$','\\$').replace('%','%%') + '"'

entry = f'''[Desktop Entry]
Type=Application
Name=ClipQueue
Comment=Collect text and screenshots, then paste in order with Ctrl+V
Exec=/usr/bin/python3 {quote_exec(base / 'clipqueue.py')}
Icon={base / 'icon.svg'}
Terminal=false
Categories=Utility;
StartupNotify=true
StartupWMClass=clipqueue.py
'''
path = applications / 'clipqueue.desktop'
path.write_text(entry)
(base / 'ClipQueue.desktop').write_text(entry)
(base / 'ClipQueue.desktop').chmod(0o755)
print(f'Installed applications menu launcher: {path}')
