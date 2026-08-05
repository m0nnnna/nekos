#!/bin/sh
# Wires up NekoShot (github.com-bound backup tool, source lives at
# N:\projects\nekoshot on the host -- a separate project from nekOS itself,
# synced to /opt/nekoshot with its own robocopy command, not part of nekOS's
# own repo sync). Run every session start, like install-apps.sh: cheap and
# idempotent, silently a no-op if /opt/nekoshot hasn't been synced yet.
#
# Host-side sync command (run from Windows, PowerShell -- not this script,
# which only runs inside the distro and has no access to N:\, a network
# share):
#   robocopy N:\projects\nekoshot \\wsl.localhost\nekos-void\opt\nekoshot /E /XO /XD .claude __pycache__ /XF README.md requirements.txt install-alpine.sh install-ubuntu.sh
set -eu

NEKOSHOT_DIR="/opt/nekoshot"
NEKOSHOT_BIN="$NEKOSHOT_DIR/nekoshot.py"

if [ ! -f "$NEKOSHOT_BIN" ]; then
    exit 0
fi

chmod +x "$NEKOSHOT_BIN"
ln -sf "$NEKOSHOT_BIN" /usr/local/bin/nekoshot
