#!/bin/sh
# Updates nekOS itself in place. Run inside the nekos-void WSL2 distro (as
# root) from anywhere -- it cd's to the repo root itself. The single source
# of truth for "how does an installed nekOS get a new version" -- called by
# windows/install.ps1 today, and meant to be the same thing a future
# nekos-software GUI "check for nekOS updates" button shells out to (see
# --check-only below), so that logic only lives in one place.
#
# Usage:
#   provision/update.sh [--channel release|master] [--check-only]
#
# --channel release (default): follow published GitHub Releases -- stable,
#   CI-and-smoke-tested versions (see RELEASING.md). This is what most
#   installs should stay on.
# --channel master: track the tip of the default branch directly -- for
#   contributors/testers who want bleeding edge, same as this repo's own
#   dev workflow.
#
# The chosen channel is remembered in ~/.config/nekos/update-channel (same
# persisted-config pattern as ~/.config/nekos/wallpaper), so future runs
# with no --channel flag reuse whatever was picked last time.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_DIR="${HOME}/.config/nekos"
CHANNEL_FILE="${CONFIG_DIR}/update-channel"
REPO_API="https://api.github.com/repos/m0nnnna/nekos"

CHANNEL=""
CHECK_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --channel)
            CHANNEL="$2"
            shift 2
            ;;
        --check-only)
            CHECK_ONLY=1
            shift
            ;;
        *)
            echo "update.sh: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [ -z "$CHANNEL" ]; then
    if [ -f "$CHANNEL_FILE" ]; then
        CHANNEL="$(cat "$CHANNEL_FILE")"
    else
        CHANNEL="release"
    fi
fi

if [ "$CHANNEL" != "release" ] && [ "$CHANNEL" != "master" ]; then
    echo "update.sh: --channel must be 'release' or 'master' (got '$CHANNEL')" >&2
    exit 1
fi

mkdir -p "$CONFIG_DIR"
echo "$CHANNEL" > "$CHANNEL_FILE"

cd "$REPO_DIR"
git fetch --tags origin >/dev/null 2>&1 || git fetch --tags origin
OLD_REF="$(git rev-parse HEAD)"
# A tagged release commit can happen to already be the tip of master (true
# right after cutting a release), so on a brand-new clone the source can
# already "match" the target ref before anything has ever been built. Only
# skip the build below if a build actually exists, not just a matching ref.
BUILT_MARKER="$REPO_DIR/wm/nekos-wm"

if [ "$CHANNEL" = "master" ]; then
    git fetch origin master
    TARGET="origin/master"
    CURRENT_MATCHES=0
    [ "$(git rev-parse HEAD)" = "$(git rev-parse "$TARGET")" ] && CURRENT_MATCHES=1

    if [ "$CHECK_ONLY" -eq 1 ]; then
        if [ "$CURRENT_MATCHES" -eq 1 ]; then
            echo "update.sh: up to date (master)"
            exit 1
        else
            echo "update.sh: update available (master, $(git rev-parse --short "$TARGET"))"
            exit 0
        fi
    fi

    if [ "$CURRENT_MATCHES" -eq 1 ] && [ -x "$BUILT_MARKER" ]; then
        echo "update.sh: already up to date (master)."
        exit 0
    fi

    if [ "$CURRENT_MATCHES" -eq 1 ]; then
        echo "update.sh: already on master, but no build found -- building..."
    else
        echo "update.sh: updating to origin/master ($(git rev-parse --short "$TARGET"))..."
        git checkout master
        git merge --ff-only origin/master
    fi
else
    LATEST_JSON="$(curl -fsS "${REPO_API}/releases/latest" 2>/dev/null || true)"
    LATEST_TAG="$(printf '%s' "$LATEST_JSON" | grep -o '"tag_name" *: *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)"$/\1/')"

    if [ -z "$LATEST_TAG" ]; then
        echo "update.sh: no published release found yet at ${REPO_API}/releases/latest" \
             "-- nothing to update to. Use --channel master to track the dev branch instead." >&2
        exit 1
    fi

    CURRENT_TAG="$(git describe --tags --exact-match 2>/dev/null || true)"

    if [ "$CHECK_ONLY" -eq 1 ]; then
        if [ "$CURRENT_TAG" = "$LATEST_TAG" ]; then
            echo "update.sh: up to date ($LATEST_TAG)"
            exit 1
        else
            echo "update.sh: update available ($LATEST_TAG)"
            exit 0
        fi
    fi

    if [ "$CURRENT_TAG" = "$LATEST_TAG" ] && [ -x "$BUILT_MARKER" ]; then
        echo "update.sh: already up to date ($LATEST_TAG)."
        exit 0
    fi

    if [ "$CURRENT_TAG" = "$LATEST_TAG" ]; then
        echo "update.sh: already on $LATEST_TAG, but no build found -- building..."
    else
        echo "update.sh: updating to $LATEST_TAG..."
        git checkout "$LATEST_TAG"
    fi
fi

echo "update.sh: building..."
if ! make; then
    echo "" >&2
    echo "update.sh: build FAILED after updating. Repo is now checked out at" \
         "$(git rev-parse --short HEAD), which may be partially built." >&2
    echo "To go back to what was working before: cd $REPO_DIR && git checkout $OLD_REF && make" >&2
    exit 1
fi

echo "update.sh: done ($(git describe --tags 2>/dev/null || git rev-parse --short HEAD))."
