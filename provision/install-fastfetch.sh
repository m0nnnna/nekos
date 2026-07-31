#!/bin/sh
# Installs a nekOS-branded fastfetch config (~/.config/fastfetch/config.jsonc)
# and wires it to auto-run in every new interactive xterm. Run from
# start-session.sh. Idempotent -- rewrites the config each launch (picking up
# any repo changes) and only appends the .bashrc hook once.
set -eu

FF_DIR="${HOME}/.config/fastfetch"
mkdir -p "$FF_DIR"

cat > "$FF_DIR/config.jsonc" <<'EOF'
{
  "$schema": "https://github.com/fastfetch-cli/fastfetch/raw/dev/doc/json_schema.json",
  "logo": {
    "type": "builtin",
    "source": "void_small",
    "color": {
      "1": "#ff8fb1",
      "2": "#edeaf5"
    },
    "padding": {
      "top": 1,
      "right": 3
    }
  },
  "display": {
    "separator": " → ",
    "color": {
      "keys": "#ff8fb1"
    }
  },
  "modules": [
    "title",
    "separator",
    { "type": "os", "format": "nekOS on {3} (WSL2)" },
    "kernel",
    "uptime",
    "packages",
    "shell",
    "wm",
    "display",
    "terminal",
    { "type": "custom", "key": "Cursor", "format": "Bibata-nekOS" },
    { "type": "custom", "key": "Icons", "format": "Papirus-Dark (pink folders)" },
    "cpu",
    "memory",
    "break",
    "colors"
  ]
}
EOF

# WSL2 leaks WSLg's own desktop session into every shell's environment
# (WAYLAND_DISPLAY + XDG_RUNTIME_DIR pointing at WSLg's compositor socket).
# fastfetch prefers Wayland detection when those are set, so left alone it
# reports WSLg's session (wrong WM, wrong/host monitor list) instead of
# nekOS's actual Xvnc/nekos-wm one. Clearing both, scoped to just this
# command, makes fastfetch fall through to X11 detection against our
# DISPLAY -- see the "GOTCHA" memory note on this if it ever needs revisiting.
HOOK='[ -n "$PS1" ] && command -v fastfetch >/dev/null && env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR fastfetch'
BASHRC="${HOME}/.bashrc"
touch "$BASHRC"
if ! grep -qF "$HOOK" "$BASHRC" 2>/dev/null; then
    printf '\n# nekOS: show the branded system-info splash in every new interactive shell.\n%s\n' "$HOOK" >> "$BASHRC"
fi
