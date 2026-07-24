# nekOS — features done

A from-scratch C/XCB/Cairo desktop environment that runs inside WSL2 (Void
Linux), nested via Xephyr `-glamor` inside WSLg's own GPU-accelerated
session and forwarded straight to a native Windows window, styled to feel
like a real, cohesive OS rather than a demo. This is a snapshot of what's
actually built and working, not a roadmap.

## Window manager & compositor (`wm/`)

- EWMH-aware compositing WM. Software-side compositor logic (COMPOSITE/
  DAMAGE/RENDER, dirty-region tracking throughout — only what actually
  changed gets repainted, never a full-screen repaint for routine window
  add/remove/raise/hide/show), but client GL/GPU rendering (e.g. Chromium)
  now runs on real hardware via Xephyr `-glamor`'s DRI3, backed by Mesa's
  d3d12 driver against WSLg's `/dev/dxg` — confirmed 2026-07-23, replacing
  the old Xvnc-era llvmpipe-only path.
- Window types: normal (titlebar + decorations), dock (top strut, e.g. the
  bar), desktop (undecorated, bottom of stack, e.g. wallpaper+icons).
- Titlebar: live title text, paw-button hover states, close/maximize/minimize.
- Open animation (fade + scale-in with a slight bounce), minimize/close
  animation (fade + shrink out).
- Interactive move and resize by dragging the titlebar/edges. Resize is
  throttled to the compositor's own paint rate so a fast drag doesn't hammer
  the compositor pixmap on every mouse pixel — always lands exactly where the
  pointer let go, never a visible lag or snap-back.
- Maximize via button, double-click titlebar, or drag-to-top-edge — one
  shared code path.
- Windows-style edge-snapping: drag to the left/right screen edge for a
  live translucent preview and a half-screen snap on release; drag a
  snapped/maximized window's titlebar to instantly restore it to its original
  size, following the cursor at the same relative grab point.
- Window cycling via Ctrl+Tab (Alt+Tab is intercepted by the Windows host).
- Click-to-focus from chrome or content area.
- RandR-aware: reflows maximized/snapped windows and re-centers decorative
  frames when the host window (Xephyr's `-resizeable` tracking the WSLg
  window) resizes the desktop.
- Themed Xcursor (Bibata-nekOS, custom-recolored) published as root
  resources.

## Bar (`bar/`)

Top dock: Menu button (opens the launcher), live window task-pills with
hover/active highlighting, clock, package-update badge, power/shutdown.
Its popup-menu component is shared by the desktop and file manager.

## Desktop (`desktop/`)

Wallpaper + `~/Desktop` icon grid with hover/selection, lazy-loaded and
mtime-cached image thumbnails, cell-local repaints. Right-click menu
(Terminal/Files/Settings). Wallpaper choice persists across restarts.

## App launcher (`launch/`)

dmenu-style: type to filter, arrow keys to navigate, Enter to launch, click
to launch, Escape to cancel. Reads `.desktop` files (user overrides system),
resolves icons through the Papirus-Dark theme with a hicolor/pixmaps
fallback chain.

## Notifications (`notify/`)

`nekos-notify "Title" "Body"` sends a toast to a background daemon over a
Unix socket. Auto-dismiss after 5s or click-dismiss.

## Settings (`settings/`)

Sidebar app: wallpaper picker (thumbnail grid) and an About page.

## Software (`software/`)

Native xbps package manager GUI — Updates/Installed/Search tabs, async
operations streamed into a log strip. A headless check loop toasts on new
updates and drives the bar's update-count badge.

## File manager (`files/`)

- Directory browsing: icon+name list, double-click to enter a folder,
  clickable breadcrumb path segments, up button, lazy per-row image
  thumbnails.
- Right-click context menu: Open, Copy, Cut, Rename, New Folder, Delete —
  shares the bar's popup-menu component.
- Rename/New Folder use a blocking text-input prompt (own small component,
  modeled on the launcher's search box).
- **Trash**: Delete moves items into `~/.local/share/nekos/trash`
  (freedesktop.org-spec-inspired files/info split with `.trashinfo`
  sidecars) instead of removing them outright. Open Trash, Restore
  (recreates missing parent directories, refuses to clobber an existing
  file), Delete Permanently, and Empty Trash (with an item-count confirm)
  round it out.
- **Copy/Cut/Paste**: single-item clipboard; copy recursively walks files
  and whole directory trees, cut is an instant rename; both auto-uniquify
  on a name collision instead of clobbering.
- **Type-to-filter**: typing while the window has focus live-filters the
  current listing by substring match (dmenu/nekos-launch style), shown as a
  clearable chip in the header; Escape or clicking the chip clears it.
  Cleared on real navigation, preserved across a same-directory refresh.
- **Hidden files**: off by default; "Show/Hide Hidden Files" in the
  empty-area right-click menu toggles dotfiles in the listing.
- **Archives**: "Compress to .tar.gz" on any file or folder (via `tar`, no
  shell involved); "Extract Here" on a recognized `.zip`/`.tar(.gz|.bz2|.xz)`
  (via `unzip`/`tar`). Extraction stages into a scratch dir first so a
  single-top-level-entry archive (the common case) lands directly under its
  own name rather than double-nesting, and a loose-files archive gets
  gathered into one new folder instead of scattering across the current
  directory.
- **Properties**: a read-only panel (name, type, size, `rwxrwxrwx` +
  octal permissions, owner/group, modified time) via "Properties..." on any
  entry.

## Text editor (`editor/`)

`nekos-edit [path]` — a plain-text editor, dark/pink-themed to match the rest
of the desktop: line-number gutter, status bar (line/column, filename,
unsaved-changes dot on the WM titlebar), monospace rendering, mouse
click-to-position and wheel scroll. Ctrl+S saves; a new/untitled buffer (or
Ctrl+Shift+S) opens a "Save As" path prompt in the same style as the file
manager's rename dialog. `nekos-open` routes dotfiles and known text/source/
config extensions here instead of `$EDITOR` in an xterm; genuinely unknown
extensions still fall back to vi (safe default for anything that might be
binary). Text selection (Shift+movement or mouse drag, highlighted inline)
with Copy/Cut/Paste (Ctrl+C/X/V) backed by the real X11 CLIPBOARD selection --
nekos-edit becomes the selection owner on copy/cut and answers other apps'
requests, and paste pulls from whoever currently owns it, so it interops with
Chromium/Firefox/xterm, not just itself. Ctrl+A selects all.

## Backups (`shot/`)

`nekos-shot` — a GUI front end for [NekoShot](https://github.com/m0nnnna/NekoShot)
(a separate, public project), the neko-powered live system-snapshot/backup
tool, deployed into the image at `/opt/nekoshot` by its own installer via
`provision/install-companions.sh`. Mirrors
NekoShot's own terminal menu one-to-one as sidebar pages rather than
reimplementing any backup/restore logic — every page just builds the
equivalent `nekoshot ...` command and either runs it (streamed into a log
strip + toast on completion, same async pattern as `nekos-software`'s
xbps operations) or reads its `--json` output straight into the page (a tiny
hand-rolled field extractor, no JSON library linked):

- **Back Up**: pick a kind (whole system / Docker-only / databases-only),
  kind-specific options (Docker: pause-containers + prune-images with a
  red-flagged "prune ALL" warning; databases: SQLite paths), a destination
  (blank defers to NekoShot's own configured default), optional headless
  mode, then runs it.
- **Schedule**: the same kind picker plus a frequency preset (hourly through
  monthly, or a custom cron expression) and a job name, calling nekoshot's
  new `schedule add` subcommand (added to NekoShot itself for this — the
  interactive menu previously called its cron-writing code directly with no
  scriptable entry point).
- **Jobs**: lists installed cron jobs with a Remove button, and flags when no
  cron daemon is actually running to fire them.
- **Status**: live-polls `nekoshot status --all --json` once a second while
  the page is open, rendering each run's phase/progress bar/ETA/throughput.
- **Snapshots**: per-kind (or everything) listing of existing snapshots;
  clicking a row fills its path into Restore.
- **Restore**: a path field and a two-step Confirm/Cancel arm (this is
  destructive, so it never fires on a single click).

## Desktop pet (`pet/`)

A cairo-drawn cat living on a true-alpha override-redirect window: chases
the cursor, sits when close, sleeps when idle, hearts on click.

## Session & theming

`provision/start-session.sh` / `stop-session.sh` bring up Xephyr `-glamor`
(nested inside WSLg's own GPU-accelerated Weston/Xwayland session, forwarded
straight to a native Windows window — no VNC viewer), a per-session D-Bus
bus, the cursor/icon theme environment, the WM and every tray component, and
the update-check loop — a clean, repeatable session lifecycle. `common/theme.h`
is the single shared palette/type-scale/spacing header every component
includes, so the whole desktop reads as one visual system (Bibata-nekOS
cursors, Papirus-Dark icons with pink folders).

xterm gets the same treatment: a themed `assets/Xresources` (dark
background, full 16-color ANSI palette pulled from `theme.h`) merged in via
`xrdb` at session start, and every new terminal opens into a nekOS-branded
[fastfetch](https://github.com/fastfetch-cli/fastfetch) splash — a
recolored Void logo, and real session info (WM correctly detected as
`nekos-wm`, the actual Xvnc display/resolution, cursor/icon theme) rather
than the WSLg host session fastfetch would show by default.

## Known gaps (not done yet)

- Multi-select in the file manager (copy/cut/delete are single-item today).
- No visual Alt+Tab switcher (Ctrl+Tab cycles focus silently).
- No quarter-screen (corner) snapping, only halves and full maximize.
- No workspaces/virtual desktops — `_NET_NUMBER_OF_DESKTOPS`/`_NET_CURRENT_DESKTOP`
  atoms are interned for EWMH compliance but the session is a single fixed
  desktop.
- Single-monitor only (RandR is used to reflow on host-window resize, not for
  multi-output layout) — moot while nested under a single WSLg window, but
  would need real work if that ever changes.
