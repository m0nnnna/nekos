# nekOS

A from-scratch C/XCB/Cairo desktop environment that runs *inside* WSL2
(Void Linux), nested via Xephyr `-glamor` inside WSLg's own
GPU-accelerated session and forwarded straight to a native Windows window —
no VNC viewer, no VM, real GPU rendering. Styled to feel like a real,
cohesive OS: its own window manager, compositor, bar, launcher, file
manager, text editor, settings app, package-manager GUI, notifications, and
a cat that lives on your desktop.

![nekOS](ascii-art.png)

See [FEATURES.md](FEATURES.md) for the full, current feature list (this is
a snapshot of what's actually built, not a roadmap).

Also included:
- [NekoShot](https://github.com/m0nnnna/NekoShot) — a live system
  snapshot/backup tool, with a native GUI front end (`nekos-shot`).
- [NekoPlayer](https://github.com/m0nnnna/nekoplayer) — a retro
  neko-themed Flutter music player.

Both install from prebuilt binaries by default (CI builds and attaches them
to each nekOS release), not from source, so a normal install never touches
the multi-GB Flutter SDK NekoPlayer would otherwise need to build from
scratch. Building from source is still available as a fallback
(`provision/install-companions.sh --from-source`) for older releases that
predate this, non-`x86_64` machines, or the `master`/dev channel.

## Requirements

- Windows 11, or Windows 10 21H2+ with WSLg support.
- WSL2 with virtualization enabled (the installer will turn this on for you
  if it's off, and may ask for a reboot).
- A few GB of free disk space and an internet connection for a normal
  (prebuilt-companions) install; more like 10-15 GB if you're on the
  `master` channel or otherwise end up on the from-source fallback path,
  which builds the Flutter SDK locally.
- A GPU with a working Windows driver (WSLg's `/dev/dxg` passthrough uses
  it for real hardware-accelerated rendering, including in the browser).

## Install

Open PowerShell and run:

```powershell
irm https://raw.githubusercontent.com/m0nnnna/nekos/master/windows/install.ps1 -OutFile install.ps1
./install.ps1
```

(Download-then-run rather than piping straight into `iex`, so you can read
the script first if you want to.) This will:

1. Check/enable WSL2 and import a clean, official Void Linux base image as
   a new `nekos-void` distro (does not touch any other WSL distro you have).
2. Install the baseline package set (browser, media player, terminal,
   dev toolchain, etc. — see `provision/setup-void.sh` for the exact list).
3. Clone and build nekOS from source, and install NekoShot + NekoPlayer
   (prebuilt by default — see above).
4. Add a "nekOS" shortcut to your Start Menu.

Once it's done, launch nekOS from the Start Menu, or run
`windows/launch.ps1` directly.

To update later, just re-run `install.ps1` — it's safe to run again and will
pull + rebuild nekOS and its companion apps instead of reimporting.

## Uninstall

```powershell
wsl --unregister nekos-void
```

This only removes the `nekos-void` WSL distro; nothing else on your system
is touched.

## License

nekOS's own code is MIT-licensed (see [LICENSE](LICENSE)). It bundles/uses
some GPL-3.0 third-party assets (a recolored Bibata cursor theme, Papirus
icons) — see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
