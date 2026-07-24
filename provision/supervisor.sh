#!/bin/sh
# Watches the core nekOS session components and restarts any that die
# unexpectedly, so a crash doesn't leave the user stuck with e.g. a dead bar
# until they manually run stop-session.sh + start-session.sh.
#
# Started by start-session.sh (setsid, backgrounded) with DISPLAY_NUM as $1.
# Killed by stop-session.sh via the "provision/supervisor.sh" marker in its
# own argv -- must die BEFORE the components it watches, or it fights the
# intentional shutdown by respawning them.
set -u

DISPLAY_NUM="${1:-:1}"
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)

WM_BIN="$REPO/wm/nekos-wm"
DESKTOP_BIN="$REPO/desktop/nekos-desktop"
BAR_BIN="$REPO/bar/nekos-bar"
NOTIFY_BIN="$REPO/notify/nekos-notify"
PET_BIN="$REPO/pet/nekos-pet"

# pgrep/pkill match patterns -- deliberately just the basename (mirroring
# stop-session.sh's own pkill patterns), NOT the resolved $XXX_BIN absolute
# paths above. start-session.sh launches these via a relative path
# ("provision/../bar/nekos-bar"), so matching against our own absolute
# $REPO-based path never lines up with the running process's argv and every
# liveness check silently fails open, spawning duplicates of everything.
WM_PAT="nekos-wm"
DESKTOP_PAT="nekos-desktop"
BAR_PAT="nekos-bar"
PET_PAT="nekos-pet"
NOTIFY_PAT="nekos-notify --daemon"

CHECK_INTERVAL=5
LOG=/tmp/nekos-supervisor.log

log() {
    printf '%s supervisor: %s\n' "$(date '+%H:%M:%S')" "$1" >>"$LOG"
}

start_desktop() { [ -x "$DESKTOP_BIN" ] && setsid env DISPLAY="$DISPLAY_NUM" "$DESKTOP_BIN" >>/tmp/nekos-desktop.log 2>&1 </dev/null & }
start_bar()     { [ -x "$BAR_BIN" ]     && setsid env DISPLAY="$DISPLAY_NUM" "$BAR_BIN"     >>/tmp/nekos-bar.log 2>&1 </dev/null & }
start_notify()  { [ -x "$NOTIFY_BIN" ]  && setsid env DISPLAY="$DISPLAY_NUM" "$NOTIFY_BIN" --daemon >>/tmp/nekos-notify.log 2>&1 </dev/null & }
start_pet()     { [ -x "$PET_BIN" ] && [ ! -e "${HOME}/.config/nekos/pet-disabled" ] && setsid env DISPLAY="$DISPLAY_NUM" "$PET_BIN" >>/tmp/nekos-pet.log 2>&1 </dev/null & }

while :; do
    sleep "$CHECK_INTERVAL"

    if [ -x "$WM_BIN" ] && ! pgrep -f "$WM_PAT" >/dev/null 2>&1; then
        # nekos-wm never sets an X CloseDownMode (defaults to DestroyAll), so
        # when its connection dies the server destroys every window it
        # created -- including the frames every other component's window was
        # reparented into. That takes the shell's own windows down with it
        # (the processes survive, attached to a destroyed window), so a WM
        # crash needs the whole shell relaunched, not just the WM. Windows
        # belonging to apps the user had open (files, editor, etc.) are lost
        # the same way a manual stop/start loses them today -- restarting
        # those automatically is out of scope here.
        log "nekos-wm died, restarting WM + shell"
        setsid env DISPLAY="$DISPLAY_NUM" "$WM_BIN" >>/tmp/nekos-wm.log 2>&1 </dev/null &
        sleep 1
        pkill -f "$DESKTOP_PAT" 2>/dev/null || true
        pkill -f "$BAR_PAT" 2>/dev/null || true
        pkill -f "$NOTIFY_PAT" 2>/dev/null || true
        pkill -f "$PET_PAT" 2>/dev/null || true
        start_desktop
        start_bar
        start_notify
        start_pet
        continue
    fi

    # WM is alive, so only its own crash cascades to the others -- check each
    # independently.
    if [ -x "$DESKTOP_BIN" ] && ! pgrep -f "$DESKTOP_PAT" >/dev/null 2>&1; then
        log "nekos-desktop died, restarting"
        start_desktop
    fi
    if [ -x "$BAR_BIN" ] && ! pgrep -f "$BAR_PAT" >/dev/null 2>&1; then
        log "nekos-bar died, restarting"
        start_bar
    fi
    if [ -x "$NOTIFY_BIN" ] && ! pgrep -f "$NOTIFY_PAT" >/dev/null 2>&1; then
        log "nekos-notify died, restarting"
        start_notify
    fi
    if [ -x "$PET_BIN" ] && [ ! -e "${HOME}/.config/nekos/pet-disabled" ] && ! pgrep -f "$PET_PAT" >/dev/null 2>&1; then
        log "nekos-pet died, restarting"
        start_pet
    fi
done
