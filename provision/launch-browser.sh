#!/bin/sh
# Launches Chromium under the nekOS session (Xephyr -glamor, nested in WSLg).
#
# GL backend: Xephyr -glamor provides a real GLX backed by Mesa's d3d12 driver
# (the host GPU via /dev/dxg), so Chromium's existing ANGLE GL backend
# (--use-gl=angle --use-angle=gl) now rides actual hardware instead of
# software llvmpipe -- verified 2026-07-23 (clean launch, no GPU/ANGLE/GLX
# errors or swiftshader fallback in the log). No flag change was needed; only
# what's underneath GLX changed. The older ANGLE-Vulkan-on-lavapipe path
# (VK_ICD_FILENAMES + --use-angle=vulkan) was needed for XLibre's Xvfb, which
# had NO GLX at all -- long gone, see ticket nekos#5 for that migration.
#
# Stale profile lock: the baked image carries a Chromium SingletonLock symlink
# from the machine it was built on (e.g. `main-laptop-2991`). On any other host
# Chromium sees that as a "foreign" lock, refuses to start, and leaves a dead
# window stuck on screen. The session only ever runs one Chromium, so clearing
# the singleton lock files up front means a stale lock can never block startup.
rm -f "${HOME}/.config/chromium/SingletonLock" \
      "${HOME}/.config/chromium/SingletonCookie" \
      "${HOME}/.config/chromium/SingletonSocket" 2>/dev/null

exec chromium --no-sandbox --disable-dev-shm-usage --use-gl=angle --use-angle=gl "$@"
