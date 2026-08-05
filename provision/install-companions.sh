#!/bin/sh
# Run inside the nekos-void WSL2 distro (as root) to install nekOS's
# companion apps -- NekoShot (backups) and NekoPlayer (music player).
#
# Default (fast) path: download the prebuilt binaries CI attaches to the
# currently-checked-out nekOS release (see .github/workflows/release.yml)
# and extract them straight into /opt/nekoshot /opt/nekoplayer. No Flutter
# SDK, no compilers, no multi-GB toolchain on the end user's machine.
#
# Falls back to the old clone-and-build-from-source path (each app's own
# self-contained nekos/install-nekos.sh) when there's nothing prebuilt to
# grab: --from-source was passed, this isn't x86_64, nekOS itself isn't on
# an exact release tag (the master/dev channel), or the tag predates this
# feature / hasn't finished uploading assets yet (e.g. v0.0.1).
#
# Either way, the lightweight session-start wiring (PATH symlink, .desktop
# entry) already lives in install-nekoshot.sh / install-nekoplayer.sh and
# needs no changes -- it just wires up whatever's present in /opt/, however
# it got there.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: please run as root (sudo sh provision/install-companions.sh)" >&2
    exit 1
fi

FROM_SOURCE=0
for arg in "$@"; do
    case "$arg" in
        --from-source) FROM_SOURCE=1 ;;
        *) echo "install-companions.sh: unknown argument: $arg" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

NEKOSHOT_REPO="https://github.com/m0nnnna/NekoShot.git"
NEKOPLAYER_REPO="https://github.com/m0nnnna/nekoplayer.git"
NEKOSHOT_SRC="${HOME}/nekoshot"
NEKOPLAYER_SRC="${HOME}/nekoplayer"

ARCH="$(uname -m)"
TAG=""
if [ "$FROM_SOURCE" -eq 0 ] && [ "$ARCH" = "x86_64" ]; then
    TAG="$(git -C "$REPO_DIR" describe --tags --exact-match 2>/dev/null || true)"
fi

clone_or_update() {
    repo="$1"
    dir="$2"
    if [ -d "$dir/.git" ]; then
        git -C "$dir" pull --ff-only
    else
        git clone --depth 1 "$repo" "$dir"
    fi
}

# $1 = asset basename (nekoshot | nekoplayer), $2 = install dir. Returns
# success if a prebuilt tarball for $TAG existed and was extracted.
try_prebuilt() {
    name="$1"
    dest="$2"
    url="https://github.com/m0nnnna/nekos/releases/download/${TAG}/${name}-linux-x86_64.tar.gz"
    tmp="$(mktemp)"
    if curl -fsSL -o "$tmp" "$url" 2>/dev/null; then
        rm -rf "$dest"
        mkdir -p "$dest"
        if tar -C "$dest" -xzf "$tmp"; then
            rm -f "$tmp"
            return 0
        fi
    fi
    rm -f "$tmp"
    return 1
}

echo "==> NekoShot"
if [ -n "$TAG" ] && try_prebuilt nekoshot /opt/nekoshot; then
    echo "installed prebuilt ($TAG)."
else
    [ -n "$TAG" ] && echo "no prebuilt asset for $TAG -- building from source instead."
    clone_or_update "$NEKOSHOT_REPO" "$NEKOSHOT_SRC"
    sh "$NEKOSHOT_SRC/nekos/install-nekos.sh"
fi

echo
echo "==> NekoPlayer"
if [ -n "$TAG" ] && try_prebuilt nekoplayer /opt/nekoplayer; then
    echo "installed prebuilt ($TAG)."
else
    [ -n "$TAG" ] && echo "no prebuilt asset for $TAG -- building from source instead."
    clone_or_update "$NEKOPLAYER_REPO" "$NEKOPLAYER_SRC"
    sh "$NEKOPLAYER_SRC/nekos/install-nekos.sh"
fi

echo
echo "install-companions.sh: done."
