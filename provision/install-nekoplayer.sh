#!/bin/sh
# Wires up NekoPlayer (Flutter Linux desktop build; source lives at
# N:\projects\nekoplayer on the host -- a separate project from nekOS itself,
# built inside the distro and synced to /opt/nekoplayer, not part of nekOS's
# own repo sync). Run every session start, like install-nekoshot.sh: cheap
# and idempotent, silently a no-op if /opt/nekoplayer hasn't been built yet.
#
# Host-side sync command (run from Windows, PowerShell -- not this script,
# which only runs inside the distro and has no access to N:\, a network
# share):
#   robocopy N:\projects\nekoplayer \\wsl.localhost\nekos-void\root\nekoplayer /E /XD .dart_tool build .idea .git android
# Then inside the distro (PATH needs /opt/flutter/bin):
#   cd /root/nekoplayer && flutter pub get && flutter build linux --release
#   mkdir -p /opt/nekoplayer && cp -a build/linux/x64/release/bundle/. /opt/nekoplayer/
set -eu

NEKOPLAYER_DIR="/opt/nekoplayer"
NEKOPLAYER_BIN="$NEKOPLAYER_DIR/nekoplayer"
APPS="${HOME}/.local/share/applications"

if [ ! -x "$NEKOPLAYER_BIN" ]; then
    exit 0
fi

mkdir -p "$APPS"
cat > "$APPS/nekoplayer.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=NekoPlayer
Comment=Retro neko-themed music player
Icon=multimedia-audio-player
Exec=$NEKOPLAYER_BIN
Terminal=false
Categories=AudioVideo;Audio;Player;
EOF
