#!/bin/sh
# Run inside the nekos-void WSL2 distro, after setup-void.sh.
# Clones and builds Xlibre (X11Libre/xserver), targeting only the Xvfb DDX
# (the in-memory framebuffer server) since this runs headless under WSL2.
set -eu

SRC_DIR="${HOME}/src/xlibre-xserver"
PREFIX="/usr/local"

if [ ! -d "${SRC_DIR}" ]; then
    git clone https://github.com/X11Libre/xserver.git "${SRC_DIR}"
fi

cd "${SRC_DIR}"
git pull --ff-only || true

MESON_OPTS="--prefix=${PREFIX} -Dxvfb=true -Dxorg=false -Dxwin=false -Dxquartz=false -Dxnest=false"

if [ -d build ]; then
    meson setup build ${MESON_OPTS} --reconfigure
else
    meson setup build ${MESON_OPTS}
fi

ninja -C build
sudo ninja -C build install

# Void's xkeyboard-config/xkbcomp packages install under the /usr prefix, but
# this build's --prefix=/usr/local means Xvfb looks for keymap data and the
# xkbcomp binary under /usr/local. Symlink them in rather than rebuilding
# xkeyboard-config from source.
mkdir -p "${PREFIX}/share/X11"
ln -sfn /usr/share/X11/xkb "${PREFIX}/share/X11/xkb"
mkdir -p /usr/share/X11/xkb/compiled
ln -sf /usr/sbin/xkbcomp "${PREFIX}/bin/xkbcomp"

echo "build-xlibre.sh: done. Sanity check by running: Xvfb :1 -screen 0 1920x1080x24"
