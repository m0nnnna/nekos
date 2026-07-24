#!/bin/sh
# Run inside the nekos-void WSL2 distro. Installs everything needed to build
# Xlibre's Xvfb DDX and serve it over VNC.
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

# NekoPlayer (a separately-sourced Flutter app, see install-nekoplayer.sh) is
# built with a native Linux Flutter SDK, not the Windows one mounted at
# /mnt/c/flutter -- that copy's shell scripts have CRLF line endings and
# break under WSL bash. Clone it once if missing:
if [ ! -d /opt/flutter ]; then
    git clone --depth 1 -b stable https://github.com/flutter/flutter.git /opt/flutter
fi
git config --global --add safe.directory /opt/flutter
export PATH="/opt/flutter/bin:$PATH"
grep -q 'opt/flutter/bin' /root/.bashrc || echo 'export PATH="/opt/flutter/bin:$PATH"' >> /root/.bashrc
flutter config --enable-linux-desktop >/dev/null 2>&1 || true

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

# Chromium runs through ANGLE's GL backend under the session's Xvnc server
# (which provides a working GLX via Mesa's software llvmpipe) -- see
# provision/launch-browser.sh. The vulkan-loader/mesa-vulkan-lavapipe/
# Vulkan-Tools packages below were needed for the OLD XLibre-Xvfb server, which
# had no GLX and so drove Chromium via ANGLE-Vulkan-on-lavapipe instead; that
# path fails on Xvnc and is no longer used. They're kept installed (harmless,
# and still useful if you boot the XLibre Xvfb stack for GLX-less experiments).
#
# NOTE: as of 2026-07-21, the fully-provisioned nekos-void distro (this
# script's output + build-xlibre.sh + the repo synced to ~/nekos) is also
# available as a portable WSL export -- see the "New machine setup" memory
# checklist for the exported-image path and import command, which is faster
# than re-running this script from scratch on a new machine.

echo "setup-void.sh: done. XLibre's dependency list isn't exhaustively documented" \
     "upstream -- if 'meson setup' below reports missing deps, install them with" \
     "xbps-install and re-run."
