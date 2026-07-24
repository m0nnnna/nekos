#!/bin/sh
# Run once inside the nekos-void WSL2 distro (as root) to install nekOS's
# companion apps -- NekoShot (backups) and NekoPlayer (music player) -- from
# their own public repos. Each ships a self-contained nekos/install-nekos.sh
# that installs its own dependencies, builds it, and wires it into the
# session (desktop entry / PATH symlink). This script just clones/updates
# each repo under ~/ (next to ~/nekos) and re-runs its installer, so it also
# doubles as the update path: `sh provision/install-companions.sh` again
# after a `git pull` in either repo picks up new commits.
#
# NOT run automatically every session (unlike install-nekoshot.sh /
# install-nekoplayer.sh, which just wire up whatever's already installed) --
# NekoPlayer's installer clones and builds the Flutter SDK, which is slow and
# only needs to happen once (or when you want to update).
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: please run as root (sudo sh provision/install-companions.sh)" >&2
    exit 1
fi

NEKOSHOT_REPO="https://github.com/m0nnnna/NekoShot.git"
NEKOPLAYER_REPO="https://github.com/m0nnnna/nekoplayer.git"
NEKOSHOT_SRC="${HOME}/nekoshot"
NEKOPLAYER_SRC="${HOME}/nekoplayer"

clone_or_update() {
    repo="$1"
    dir="$2"
    if [ -d "$dir/.git" ]; then
        git -C "$dir" pull --ff-only
    else
        git clone --depth 1 "$repo" "$dir"
    fi
}

echo "==> NekoShot"
clone_or_update "$NEKOSHOT_REPO" "$NEKOSHOT_SRC"
sh "$NEKOSHOT_SRC/nekos/install-nekos.sh"

echo
echo "==> NekoPlayer"
clone_or_update "$NEKOPLAYER_REPO" "$NEKOPLAYER_SRC"
sh "$NEKOPLAYER_SRC/nekos/install-nekos.sh"

echo
echo "install-companions.sh: done."
