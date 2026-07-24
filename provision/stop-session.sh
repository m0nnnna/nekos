#!/bin/sh
# Tears down the nekOS session (Xephyr/nekos-wm/nekos-bar) started by
# start-session.sh -- only this session, not WSL or the host Windows machine.
# Mirrors start-session.sh's own DISPLAY_NUM value; keep both in sync if that
# ever changes. Invoked by nekos-bar's "Shutdown" app-menu entry.
set -eu

pkill -f "nekos-notify --daemon" 2>/dev/null || true
pkill -f "nekos-update-check" 2>/dev/null || true
pkill -f nekos-bar 2>/dev/null || true
pkill -f nekos-desktop 2>/dev/null || true
pkill -f nekos-wm 2>/dev/null || true
pkill -f xdg-desktop-portal-gtk 2>/dev/null || true
pkill -f /usr/libexec/xdg-desktop-portal 2>/dev/null || true
# The per-session D-Bus started by start-session.sh (matched by its socket path).
pkill -f "dbus-daemon.*nekos-session" 2>/dev/null || true
pkill -f "Xephyr.*:1" 2>/dev/null || true
# Legacy servers, in case an old session predates the Xephyr switch.
pkill -f "Xvnc :1" 2>/dev/null || true
pkill -f "x0vncserver.*5901" 2>/dev/null || true
pkill -f "x11vnc.*5901" 2>/dev/null || true
pkill -f "Xvfb :1" 2>/dev/null || true
