#!/bin/sh
# Run inside the nekos-void WSL2 distro. Installs the baseline package set
# nekOS itself needs to build and run (window manager/compositor toolchain,
# Chromium, mpv, dev tools). NekoShot and NekoPlayer bring their own
# dependencies via provision/install-companions.sh.
#
# xorg-server-xephyr/mesa-dri/libgbm/pulseaudio-utils were found missing
# here (2026-07-24) via a real fresh-install bug report: the nested X server
# (Xephyr) itself was never actually in this list, only referenced in
# comments -- it had been installed manually on the dev machine ages ago,
# during the original Xvnc-to-Xephyr migration, and that never made it back
# into this script. mesa-dri/libgbm (the GALLIUM_DRIVER=d3d12 driver +
# buffer management start-session.sh's glamor path depends on) and
# pulseaudio-utils (audio for mpv/NekoPlayer) were also manually installed
# at some point and missing here -- added defensively since a missing GPU
# driver fails silently (falls back to software rendering) rather than
# loudly like the missing Xephyr binary did.
#
# sakura replaced xterm (2026-07-24, nekos#36): xterm's default keybindings
# have no Ctrl+Shift+C/V copy-paste (X primary-selection + middle-click
# only), and the *system* xterm.desktop the launcher picked up ran bare
# `xterm` -- root's login shell is /bin/sh (dash, see nekos-desktop.c's old
# comment), which has no readline, so that specific launch path had no
# command history or tab-completion either. sakura is libvte-based (real
# Ctrl+Shift+C/V, scrollback, standard readline-friendly PTY behavior) and
# every launch point now explicitly runs bash (see desktop/nekos-desktop.c,
# launch/nekos-launch.c, provision/install-apps.sh) so there's exactly one
# definition of "how to launch nekOS's terminal" instead of the desktop
# menu and the launcher disagreeing.
set -eu

xbps-install -Sy \
    base-devel \
    meson \
    ninja \
    git \
    xorg-server-xephyr \
    mesa-dri \
    libgbm \
    pulseaudio-utils \
    libxcb-devel \
    xcb-util-devel \
    pixman-devel \
    freetype-devel \
    font-util \
    tigervnc \
    xrandr \
    sakura \
    xclock \
    fastfetch \
    libX11-devel \
    libxcvt-devel \
    libXfont2-devel \
    libxkbfile-devel \
    nettle-devel \
    libepoxy-devel \
    libxshmfence-devel \
    xkeyboard-config \
    xkbcomp \
    font-misc-misc \
    font-cursor-misc \
    dejavu-fonts-ttf \
    cairo-devel \
    xcb-util-renderutil-devel \
    xcb-util-keysyms-devel \
    xcb-util-cursor-devel \
    papirus-icon-theme \
    xdpyinfo \
    xprop \
    xdotool \
    imlib2 \
    imlib2-devel \
    imlib2-tools \
    dbus \
    dbus-x11 \
    chromium \
    vulkan-loader \
    mesa-vulkan-lavapipe \
    Vulkan-Tools \
    rsync \
    zstd \
    xclip \
    gtk+3-devel \
    mpv \
    mpv-devel \
    clang \
    cmake \
    curl \
    strace \
    xdg-desktop-portal \
    xdg-desktop-portal-gtk

# NekoPlayer (a separately-sourced Flutter app -- see provision/install-companions.sh)
# brings its own native Linux Flutter SDK via its own nekos/install-nekos.sh
# installer, not the Windows Flutter SDK some devs have mounted at
# /mnt/c/flutter (that copy's shell scripts have CRLF line endings and break
# under WSL bash). Nothing to do here.

# xdg-desktop-portal: needed for file_picker's GTK file-chooser dialog (used
# by NekoPlayer's "Add files"/"Add folder" -- file_picker on Linux only talks
# to the freedesktop portal, no native GTK fallback). Only xdg-desktop-portal-gtk
# is installed, and its own .portal file only self-registers under
# UseIn=gnome, so force it as the default backend regardless of
# XDG_CURRENT_DESKTOP.
mkdir -p /etc/xdg-desktop-portal
cat > /etc/xdg-desktop-portal/portals.conf <<'EOF'
[preferred]
default=gtk
EOF

# nekos-settings (the wallpaper/frame picker) watches ~/nekos-wallpapers/ for
# image files -- reachable from Windows at
# \\wsl.localhost\<distro>\root\nekos-wallpapers\ once it exists (created on
# first run of nekos-settings, or just mkdir it yourself ahead of time).

# Chromium runs through ANGLE's GL backend (--use-gl=angle --use-angle=gl,
# see provision/launch-browser.sh) against Xephyr -glamor's real DRI3/GLX,
# backed by Mesa's d3d12 driver against WSLg's /dev/dxg. The vulkan-loader/
# mesa-vulkan-lavapipe/Vulkan-Tools packages below are a leftover from an
# earlier XLibre-Xvfb-based session (no GLX, so Chromium drove ANGLE over
# Vulkan-on-lavapipe instead) that Xephyr has since replaced; kept installed
# since they're harmless and still useful for any GLX-less experiments.
# provision/build-xlibre.sh (unused by the current session) is the other
# leftover from that abandoned path.

echo "setup-void.sh: baseline install done."
