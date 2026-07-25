#!/bin/sh
# Installs a nekOS-branded fastfetch config (~/.config/fastfetch/config.jsonc)
# and wires it to auto-run in every new interactive terminal, plus a terminal
# title fix (see the second hook below). Run from start-session.sh. Idempotent
# -- rewrites the config each launch (picking up any repo changes) and only
# appends each .bashrc hook once.
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

# sakura (nekOS's terminal, see provision/setup-void.sh) defaults its WM_NAME
# to the literal string "xterm" for legacy compatibility, and nothing else
# sets a real title -- so every terminal window's titlebar misleadingly read
# "xterm" even after the xterm->sakura swap (2026-07-24, nekos#36). Standard
# OSC 0 title escape in PROMPT_COMMAND fixes it for every terminal, not just
# sakura.
TITLE_HOOK='PROMPT_COMMAND="printf \"\\033]0;Terminal\\007\""'
if ! grep -qF "$TITLE_HOOK" "$BASHRC" 2>/dev/null; then
    printf '\n# nekOS: sakura defaults to "xterm" as its window title -- set a real one.\n%s\n' "$TITLE_HOOK" >> "$BASHRC"
fi
