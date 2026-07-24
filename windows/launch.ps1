# Brings up the nekos "OS in a window" pipeline: starts Xephyr -glamor
# (nested inside WSLg's own GPU-accelerated Weston/Xwayland session) +
# nekos-wm + nekos-bar inside the nekos-void WSL2 distro. No VNC viewer is
# needed -- WSLg forwards the Xephyr window straight to the Windows desktop
# (titled "nekOS") the same way it forwards any other Linux GUI app. Resizing
# that window resizes the desktop (-resizeable makes Xephyr track its host
# window size; nekos-wm/nekos-bar reflow to match) -- see ticket nekos#5 for
# the original resize work this preserves.
#
# Requires: the nekos-void WSL2 distro provisioned (see provision/setup-void.sh),
# with this repo copied to ~/nekos inside it, and WSLg enabled (default on
# modern Windows/WSL2 -- provides /dev/dxg GPU passthrough and the Xwayland
# display Xephyr nests into).

$DistroName = "nekos-void"

wsl -d $DistroName -- sh -c "cd ~/nekos && sh provision/start-session.sh"
