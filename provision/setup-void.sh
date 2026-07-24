#!/bin/sh
# Run inside the nekos-void WSL2 distro. Installs the baseline package set
# nekOS itself needs to build and run (window manager/compositor toolchain,
# Chromium, mpv, dev tools). NekoShot and NekoPlayer bring their own
# dependencies via provision/install-companions.sh.
set -eu

xbps-install -Sy \
    base-devel \
    meson \
    ninja \
    git \
    libxcb-devel \
    xcb-util-devel \
    pixman-devel \
    freetype-devel \
    font-util \
    tigervnc \
    xrandr \
    xrdb \
    xterm \
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
