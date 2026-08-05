#!/bin/sh
# Wires up NekoMail (source lives at N:\projects\nekomail on the host -- a
# separate project from nekOS itself, synced to /opt/nekomail with its own
# robocopy command, not part of nekOS's own repo sync, same treatment as
# install-nekoshot.sh/install-nekoplayer.sh). Run every session start, like
# those two: cheap and idempotent, silently a no-op if /opt/nekomail hasn't
# been synced+installed yet (nekos/install-nekos.sh, in NekoMail's own repo,
# does that build/install step -- see NekoMail's nekos/README.md).
#
# Unlike NekoShot/NekoPlayer, NekoMail is a background service (IMAP IDLE
# watchers + poll loop), not a launch-on-demand tool -- so this script also
# starts it running, once per session, independent of whether its viewer
# window is ever opened. It is NOT added to supervisor.sh's watch list
# (matches NekoShot/NekoPlayer's launch-once treatment, not the WM/bar/
# desktop's crash-auto-restart treatment) -- a mid-session crash needs a
# manual `nekomail-server` (re)run until/unless that's revisited.
#
# Host-side sync command (run from Windows, PowerShell -- not this script,
# which only runs inside the distro and has no access to N:\, a network
# share):
#   robocopy N:\projects\nekomail \\wsl.localhost\nekos-void\opt\nekomail /E /XD .git node_modules .venv dist android-client\android
set -eu

NEKOMAIL_DIR="/opt/nekomail"
VENV_PYTHON="$NEKOMAIL_DIR/backend/.venv/bin/python"
LAUNCHER="$NEKOMAIL_DIR/backend/launcher.py"
STATIC_INDEX="$NEKOMAIL_DIR/backend/app/static/index.html"
APPS="${HOME}/.local/share/applications"

# Not installed (nekos/install-nekos.sh hasn't been run in the synced
# copy) -- nothing to do.
if [ ! -x "$VENV_PYTHON" ] || [ ! -f "$STATIC_INDEX" ]; then
    exit 0
fi

# Stale instance from a prior session (mirrors start-session.sh's own
# pkill-before-relaunch treatment of Xephyr/D-Bus at its top) -- avoids a
# duplicate server / port conflict if this script runs again this session.
pkill -f "$LAUNCHER" 2>/dev/null || true

setsid "$VENV_PYTHON" "$LAUNCHER" >/tmp/nekomail.log 2>&1 < /dev/null &

mkdir -p "$APPS"
cat > "$APPS/nekomail.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=NekoMail
Comment=Multi-account mail client
Icon=$NEKOMAIL_DIR/backend/app/static/icons/icon-192.png
Exec=nekomail
Terminal=false
Categories=Network;Email;
EOF
