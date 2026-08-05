# Third-party notices

nekOS's own source code is MIT-licensed (see `LICENSE`). The following assets
are third-party or derivatives of third-party work and remain under their
original licenses — the MIT grant above does not extend to them.

## Bibata-nekOS cursor theme (`assets/cursors/Bibata-nekOS.tar.gz`)

A recolored derivative of [Bibata Cursor](https://github.com/ful1e5/Bibata_Cursor)
(base `#2B2438` / outline `#FF8FB1`), which is licensed under the
[GNU General Public License v3.0](https://github.com/ful1e5/Bibata_Cursor/blob/main/LICENSE).
This derivative is distributed under the same GPL-3.0 license. Rebuild
instructions live in `de-components` project notes; upstream config is
Bibata's `configs/normal/x.build.toml`.

## Papirus-Dark icon theme

Not bundled in this repo — installed at provision time as the Void Linux
package `papirus-icon-theme`. [Papirus](https://github.com/PapirusDevelopmentTeam/papirus-icon-theme)
is licensed under GPL-3.0. `papirus-folders` (also invoked at provision
time to recolor folders pink) is GPL-3.0 as well.

## Other provisioned software

Everything else the installer provisions (Chromium, mpv, sakura, Flutter, the
Void base system, etc.) is installed as unmodified upstream packages/binaries
via `xbps` or upstream installers, not redistributed as part of this repo,
and remains under each project's own license.
