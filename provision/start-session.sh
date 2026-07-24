#!/bin/sh
# Run inside the nekos-void WSL2 distro. Brings up the full pipeline:
# Xephyr -glamor (nested inside WSLg's own GPU-accelerated Weston/Xwayland
# session) -> nekos-wm -> nekos-bar, all backgrounded. WSLg forwards the
# Xephyr window straight to the Windows desktop -- no VNC viewer needed.
set -eu

HOST_DISPLAY=":0"   # WSLg's own Xwayland display, provided by the platform.
DISPLAY_NUM=":1"    # The nested display Xephyr creates for nekOS's own clients.
# Initial desktop size. -resizeable makes Xephyr track its host window's size
# (the Windows window WSLg renders it into), so the desktop follows resizes
# the same way the old Xvnc+VNC-viewer setup did via SetDesktopSize.
DEFAULT_W="1600"
DEFAULT_H="900"
WM_BIN="$(dirname "$0")/../wm/nekos-wm"
DESKTOP_BIN="$(dirname "$0")/../desktop/nekos-desktop"
BAR_BIN="$(dirname "$0")/../bar/nekos-bar"
NOTIFY_BIN="$(dirname "$0")/../notify/nekos-notify"
SOFTWARE_BIN="$(dirname "$0")/../software/nekos-software"
PET_BIN="$(dirname "$0")/../pet/nekos-pet"

pkill -f "provision/supervisor.sh" 2>/dev/null || true
pkill -f "Xephyr.*${DISPLAY_NUM}" 2>/dev/null || true
# Legacy servers, in case an old session predates the Xephyr switch.
pkill -f "Xvnc ${DISPLAY_NUM}" 2>/dev/null || true
pkill -f "Xvfb ${DISPLAY_NUM}" 2>/dev/null || true
pkill -f "x0vncserver" 2>/dev/null || true
pkill -f "x11vnc" 2>/dev/null || true
# Session bus from a previous run (see the D-Bus block below).
pkill -f "dbus-daemon.*nekos-session" 2>/dev/null || true

# --- GL: real GPU via WSLg's d3d12 gallium driver ---------------------------
# Xephyr -glamor gives clients on this display real DRI3, backed by Mesa's
# d3d12 driver against /dev/dxg (WSLg's own GPU passthrough -- confirmed via
# spike: GL_RENDERER reports the real host GPU, not llvmpipe). Pinning
# GALLIUM_DRIVER=d3d12 skips Mesa's default probe order (which would otherwise
# try zink first) and lands every client, including nekos-wm and the bar,
# directly on real hardware rendering. This replaces the old Xvnc-era
# llvmpipe/LIBGL_ALWAYS_SOFTWARE pin, which existed only because Xvnc had no
# GPU path at all.
export GALLIUM_DRIVER=d3d12
export GDK_GL=disable
export GSK_RENDERER=cairo   # GTK4's equivalent, harmless where GTK4 is absent

# --- XDG_RUNTIME_DIR: needs to be owned by root, not WSLg's uid-1000 dir ----
# WSLg exports XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir globally (owned by uid
# 1000, the WSL default user), but this whole session runs as root. Apps that
# check runtime-dir ownership refuse to start against a dir they don't own --
# Firefox hard-fails with "Running Firefox as root in a regular user's session
# is not supported", naming that exact path/uid mismatch. Point every app
# spawned from this session at a root-owned runtime dir instead; also unset
# WAYLAND_DISPLAY since nekOS's own clients talk X11 to Xephyr, not WSLg's
# Wayland compositor, and leaving it set just invites the same host-vs-session
# ownership confusion.
export XDG_RUNTIME_DIR="/tmp/nekos-runtime"
mkdir -p "${XDG_RUNTIME_DIR}"
chmod 700 "${XDG_RUNTIME_DIR}"
unset WAYLAND_DISPLAY

# --- D-Bus: a working session bus ------------------------------------------
# Void's minimal install has no machine-id and no session bus, so every GTK/Qt
# app spews "Unable to acquire session bus ... Unable to load /etc/machine-id"
# and blocks on failed bus calls (portals, settings, the file chooser). Create
# a machine-id once, then start one session bus and export its address so every
# app launched from this session (the WM, bar, desktop, and anything their menus
# spawn, e.g. the browser) inherits it. All of this is best-effort: under `set -e`
# a hiccup here must never abort the whole session launch, hence the `|| true`s.
[ -s /etc/machine-id ] || dbus-uuidgen --ensure=/etc/machine-id || true
mkdir -p /var/lib/dbus || true
[ -s /var/lib/dbus/machine-id ] || cp /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    # Fixed socket path (rather than dbus-launch's random one) so the "nekos-session"
    # marker in the daemon's argv is greppable for teardown by stop-session.sh and
    # the pkill above. Connecting by bare unix:path is correct -- the server hands
    # the client its guid during the auth handshake, so none is baked into the address.
    _dbus_sock="/tmp/nekos-session-bus"
    rm -f "$_dbus_sock"
    setsid dbus-daemon --session --nopidfile --fork \
        --address="unix:path=${_dbus_sock}" >/dev/null 2>&1 < /dev/null || true
    export DBUS_SESSION_BUS_ADDRESS="unix:path=${_dbus_sock}"
fi

# --- Cursor theme: Bibata-nekOS ---------------------------------------------
# A Bibata recolor in the nekOS palette (base panel purple, pink outline),
# built from https://github.com/ful1e5/Bibata_Cursor (GPL-3.0) and stored as
# a tarball in assets/cursors/. Extracted once into ~/.icons (a standard
# Xcursor search path). The env vars steer libXcursor clients (GTK, Chromium);
# nekos-wm additionally publishes them as Xcursor.* root resources and sets
# the themed root cursor (see set_default_cursor in wm/src/main.c).
export XCURSOR_THEME="Bibata-nekOS"
export XCURSOR_SIZE=24
_cur_tar="$(dirname "$0")/../assets/cursors/Bibata-nekOS.tar.gz"
if [ ! -d "${HOME}/.icons/Bibata-nekOS" ] && [ -f "$_cur_tar" ]; then
    mkdir -p "${HOME}/.icons" || true
    tar xzf "$_cur_tar" -C "${HOME}/.icons" || true
fi

# Why Xephyr instead of Xvnc: Xvnc had no GPU path at all (pure software
# framebuffer, no GLAMOR/DRI3), so client apps -- including Chromium -- were
# stuck on llvmpipe no matter what. Xephyr -glamor is a nested X server that
# opens as an ordinary client window on the host display (WSLg's own
# Xwayland/Weston, DISPLAY=:0), and WSLg forwards that window straight to the
# Windows desktop -- no VNC viewer, no separate re-encode step. Confirmed via
# spike (2026-07-23): clients on the nested display get real GPU rendering
# (GL_RENDERER: D3D12 <host GPU>) through it, and nekos-wm runs against it
# unmodified. Before this, the session ran on Xvnc (TigerVNC's integrated X +
# VNC server); before that, Xvfb + x0vncserver (abandoned -- TigerVNC's client
# libs couldn't negotiate XLibre Xvfb's RANDR/DAMAGE extensions, so the desktop
# never resized). XLibre's Xvfb build (build-xlibre.sh) stays in the repo from
# that experiment but is unused by the session now.
#
# setsid (not just nohup) is required: nohup only blocks SIGHUP, but WSL tears
# down the whole session/process-group tied to the launching wsl.exe process
# once it exits, killing plain backgrounded children along with it. setsid
# moves the child into its own session so it survives that teardown.
# -resizeable makes Xephyr track its host window's size as WSLg's forwarded
# window is resized, the same live-resize feel the old SetDesktopSize path gave.
setsid env DISPLAY="${HOST_DISPLAY}" Xephyr -glamor -screen "${DEFAULT_W}x${DEFAULT_H}" \
    -resizeable -title nekOS "${DISPLAY_NUM}" >/tmp/xephyr.log 2>&1 < /dev/null &

# Poll until Xephyr is actually accepting connections on DISPLAY_NUM, instead
# of guessing with a fixed sleep -- a cold machine (first-ever launch, no
# warm d3d12/driver caches yet) can take longer than a fixed sleep assumes,
# which otherwise races xrdb below into "Can't open display" against a
# server that just isn't up yet.
for _i in $(seq 1 50); do
    env DISPLAY="${DISPLAY_NUM}" xdpyinfo >/dev/null 2>&1 && break
    sleep 0.2
done

# --- xdg-desktop-portal: backs file_picker's GTK file-chooser dialog -------
# file_picker (used by NekoPlayer's "Add files"/"Add folder") talks to the
# freedesktop portal over D-Bus with no native fallback on Linux, so without
# these two daemons pickFiles()/getDirectoryPath() would hang forever waiting
# on a service that never replies. Backend (xdg-desktop-portal-gtk) must be
# started before the frontend (xdg-desktop-portal) so its D-Bus name is
# already registered when the frontend dispatches to it. See setup-void.sh
# for the portals.conf that forces the gtk backend (its .portal file only
# self-registers under UseIn=gnome otherwise) and package install.
if [ -x /usr/libexec/xdg-desktop-portal-gtk ]; then
    setsid env DISPLAY="${DISPLAY_NUM}" XDG_CURRENT_DESKTOP=gtk \
        /usr/libexec/xdg-desktop-portal-gtk >/tmp/nekos-portal-gtk.log 2>&1 < /dev/null &
    sleep 1
    setsid env DISPLAY="${DISPLAY_NUM}" XDG_CURRENT_DESKTOP=gtk \
        /usr/libexec/xdg-desktop-portal >/tmp/nekos-portal.log 2>&1 < /dev/null &
fi

# Install/refresh the nekOS app .desktop entries so the launcher lists them.
sh "$(dirname "$0")/install-apps.sh" || true

# Symlink NekoShot into PATH if its source has been synced to /opt/nekoshot
# (see install-nekoshot.sh for the host-side sync command).
sh "$(dirname "$0")/install-nekoshot.sh" || true

# .desktop entry for NekoPlayer if its Linux release bundle has been built to
# /opt/nekoplayer (see install-nekoplayer.sh for the host-side sync command).
sh "$(dirname "$0")/install-nekoplayer.sh" || true

# Branded fastfetch splash for every new xterm -- see the script for why it
# needs the WAYLAND_DISPLAY/XDG_RUNTIME_DIR workaround.
sh "$(dirname "$0")/install-fastfetch.sh" || true

# Default wallpaper: on a fresh session with no wallpaper ever picked, seed the
# config with the shipped default (~/nekos-wallpapers/wallpaper.jpg) so both
# readers of the config (nekos-wm's compositor and nekos-desktop) and the
# Settings highlight all agree without needing their own fallback logic. Runs
# before the WM launches so the compositor's restore-on-start sees it.
_wp_config="${HOME}/.config/nekos/wallpaper"
_wp_default="${HOME}/nekos-wallpapers/wallpaper.jpg"
if [ ! -s "$_wp_config" ] && [ -f "$_wp_default" ]; then
    mkdir -p "${HOME}/.config/nekos" || true
    printf '%s\n' "$_wp_default" > "$_wp_config" || true
fi

if [ -x "${WM_BIN}" ]; then
    setsid env DISPLAY="${DISPLAY_NUM}" "${WM_BIN}" >/tmp/nekos-wm.log 2>&1 < /dev/null &
    sleep 1
else
    echo "start-session.sh: nekos-wm not built yet (expected at ${WM_BIN}); skipping WM launch."
fi

# xterm (nekOS's only terminal app) otherwise starts with X's stock
# black-on-white -- merge assets/Xresources so it picks up the same palette
# as everything else. One-shot, not backgrounded: xrdb just writes the
# RESOURCE_MANAGER property and exits; every xterm launched afterward reads
# it automatically, no per-launch wrapper needed. Safe to run any time after
# the xdpyinfo poll above confirms the display is actually up (deliberately
# not gated on nekos-wm being built/launched -- xterm and xrdb don't need it).
env DISPLAY="${DISPLAY_NUM}" xrdb -nocpp -merge "$(dirname "$0")/../assets/Xresources" || true

# nekos-desktop: the desktop layer (wallpaper + ~/Desktop icons + right-click
# menu). Launched after the WM so its _NET_WM_WINDOW_TYPE_DESKTOP window is
# managed (bottom of the stack) from the start. Like the bar, it relies on cwd
# being ~/nekos for the relative paths its menu spawns.
if [ -x "${DESKTOP_BIN}" ]; then
    setsid env DISPLAY="${DISPLAY_NUM}" "${DESKTOP_BIN}" >/tmp/nekos-desktop.log 2>&1 < /dev/null &
else
    echo "start-session.sh: nekos-desktop not built yet (expected at ${DESKTOP_BIN}); skipping desktop launch."
fi

# nekos-bar relies on its cwd being ~/nekos for the relative paths its app-menu
# entries spawn (settings/, provision/) -- inherited here since start-session.sh
# is itself launched from ~/nekos.
if [ -x "${BAR_BIN}" ]; then
    setsid env DISPLAY="${DISPLAY_NUM}" "${BAR_BIN}" >/tmp/nekos-bar.log 2>&1 < /dev/null &
else
    echo "start-session.sh: nekos-bar not built yet (expected at ${BAR_BIN}); skipping bar launch."
fi

# nekos-notify daemon: renders toast notifications (apps send via
# `nekos-notify "Title" "Body"` over its Unix socket).
if [ -x "${NOTIFY_BIN}" ]; then
    setsid env DISPLAY="${DISPLAY_NUM}" "${NOTIFY_BIN}" --daemon >/tmp/nekos-notify.log 2>&1 < /dev/null &
fi

# nekos-pet: the resident cat (chases the cursor, naps, accepts pets).
# Settings > Pet writes this marker to keep the cat off; it also starts/stops
# the running cat live, so this guard only matters at session start.
if [ -x "${PET_BIN}" ] && [ ! -e "${HOME}/.config/nekos/pet-disabled" ]; then
    setsid env DISPLAY="${DISPLAY_NUM}" "${PET_BIN}" >/tmp/nekos-pet.log 2>&1 < /dev/null &
fi

# Update checker: nekos-software --check syncs xbps repodata, writes the
# pending-update count to /tmp/nekos-updates (nekos-bar draws it as a badge),
# and toasts when updates first appear. First check ~2 minutes after session
# start (lets the desktop settle / network come up), then every 30 minutes.
if [ -x "${SOFTWARE_BIN}" ]; then
    pkill -f "nekos-update-check" 2>/dev/null || true
    setsid env DISPLAY="${DISPLAY_NUM}" sh -c "
        # marker for pkill: nekos-update-check
        sleep 120
        while :; do
            \"${SOFTWARE_BIN}\" --check
            sleep 1800
        done" >/tmp/nekos-update-check.log 2>&1 < /dev/null &
fi

# Supervisor: watches wm/bar/desktop/notify/pet and restarts any that die
# mid-session (see supervisor.sh -- a WM crash also takes its restart of the
# rest of the shell with it). Launched last, once everything above is already
# up, so it never mistakes still-starting components for dead ones.
setsid sh "$(dirname "$0")/supervisor.sh" "${DISPLAY_NUM}" >/tmp/nekos-supervisor.log 2>&1 < /dev/null &

echo "start-session.sh: Xephyr ${DEFAULT_W}x${DEFAULT_H} on ${DISPLAY_NUM}," \
     "nested under WSLg (${HOST_DISPLAY}) -- window should appear on the" \
     "Windows desktop directly, titled nekOS."

# Grace period: there's a brief window right after setsid where WSL's teardown
# of this exiting client can still catch the newly-detached children before
# they've fully registered as independent sessions. Without this, Xvnc/nekos-wm
# observed getting killed anyway despite setsid.
sleep 2

# Welcome toast -- also confirms the notification pipeline is up end to end.
if [ -x "${NOTIFY_BIN}" ]; then
    env DISPLAY="${DISPLAY_NUM}" "${NOTIFY_BIN}" "nyahallo! ♥" "Welcome back to nekOS. The cat missed you." || true
fi
