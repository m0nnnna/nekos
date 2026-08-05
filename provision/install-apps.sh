#!/bin/sh
# Installs .desktop entries for the nekOS apps into ~/.local/share/applications
# so nekos-launch (and any XDG-aware tool) lists them alongside system apps.
# Run from start-session.sh. Idempotent -- rewrites each launch, picking up the
# current repo location.
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
APPS="${HOME}/.local/share/applications"
mkdir -p "$APPS"

cat > "$APPS/nekos-terminal.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Terminal
Comment=Command-line shell
Icon=utilities-terminal
Exec=sakura -e /bin/bash
Terminal=false
Categories=System;TerminalEmulator;
EOF

cat > "$APPS/nekos-files.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Files
Comment=Browse files
Icon=system-file-manager
Exec=$REPO/files/nekos-files
Terminal=false
Categories=System;FileManager;
EOF

cat > "$APPS/nekos-software.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Software
Comment=Install packages and updates (xbps)
Icon=system-software-install
Exec=$REPO/software/nekos-software
Terminal=false
Categories=System;PackageManager;
EOF

cat > "$APPS/nekos-edit.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Text Editor
Comment=Edit text files
Icon=accessories-text-editor
Exec=$REPO/editor/nekos-edit %f
Terminal=false
Categories=Utility;TextEditor;
EOF

cat > "$APPS/nekos-shot.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Backups
Comment=Snapshot and restore with NekoShot
Icon=deja-dup
Exec=$REPO/shot/nekos-shot
Terminal=false
Categories=System;Utility;
EOF

cat > "$APPS/nekos-settings.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Settings
Comment=Wallpaper and appearance
Icon=preferences-desktop
Exec=$REPO/settings/nekos-settings
Terminal=false
Categories=Settings;
EOF

# Overrides the system chromium.desktop (same basename -> nekos-launch dedups
# in favor of this one) so "Browser" launches Chromium through the wrapper that
# sets the GL flags this environment needs and clears the stale profile lock.
cat > "$APPS/chromium.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Browser
Comment=Web browser (Chromium)
Icon=chromium
Exec=$REPO/provision/launch-browser.sh %U
Terminal=false
Categories=Network;WebBrowser;
EOF
