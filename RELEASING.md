# Releasing nekOS

`build-check` CI (`.github/workflows/build-check.yml`) confirms nekOS,
NekoShot, and NekoPlayer all still build from source on every push to
`master`. It cannot confirm the actual desktop session works, though --
that needs real WSLg + GPU passthrough, which no Linux CI runner has. So
every tagged release also gets a manual smoke test on a real Windows
machine (or a clean Windows VM) first.

## Pre-release smoke test

On a Windows machine that has never run nekOS before (or after
`wsl --unregister nekos-void` to simulate one):

1. `git clone https://github.com/m0nnnna/nekos.git` (or download
   `windows/install.ps1` standalone) and run `./windows/install.ps1` to
   completion, start to finish, with no manual intervention.
2. Launch nekOS from the Start Menu shortcut it creates.
3. Confirm the session comes up: bar, wallpaper/desktop icons, the pet.
4. Open each baseline app from the launcher/bar and confirm it opens
   without error:
   - Files
   - Settings
   - Software
   - Browser (confirm a page actually renders -- this is the GPU/ANGLE path)
   - Editor (`nekos-edit`, opened via the file manager or `nekos-open`)
   - Backups (`nekos-shot` -- confirm it lists NekoShot's status without
     erroring; a full capture/restore isn't necessary for a smoke test)
   - NekoPlayer (confirm it opens and the file picker works -- that's the
     xdg-desktop-portal path)
5. Resize the nekOS window and confirm the desktop reflows.
6. Power off via the bar, then re-launch to confirm session teardown/
   restart is clean (`provision/stop-session.sh` / `start-session.sh`).
7. Re-run `./windows/install.ps1` a second time against the now-existing
   install and confirm it updates/rebuilds instead of erroring, and lands on
   the latest published release tag (not just `master` HEAD).
8. `./windows/install.ps1 -Channel master` and confirm it switches onto
   `master` instead; re-run with `-Channel release` (or no flag -- it's the
   default) and confirm it switches back to the latest tag.
9. Once `release.yml`'s `companion-assets` job has finished (see below),
   re-run `sh provision/install-companions.sh` inside `nekos-void` and
   check the log: it should say "installed prebuilt (vX.Y.Z)" for both
   NekoShot and NekoPlayer, not clone the Flutter SDK. Confirm both apps
   still launch fine afterward.

If anything in this list fails, fix it and re-run the whole checklist --
don't cherry-pick just the failing step, since fixes can have side effects
elsewhere in the session lifecycle.

## Cutting the release

Once the smoke test passes and CI is green on `master`:

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

Pushing the tag is the whole release step: `.github/workflows/release.yml`
picks up any `v*.*.*` tag push and runs `gh release create --generate-notes`
automatically, publishing it as a GitHub Release with auto-generated notes
from the commits since the last tag. nekOS itself always builds from source
on the end user's machine (`install.ps1`/`provision/update.sh`), so no
binary asset is needed for nekOS -- but the same workflow also builds
NekoShot and NekoPlayer in a Void Linux container and uploads
`nekoshot-linux-x86_64.tar.gz` / `nekoplayer-linux-x86_64.tar.gz` as release
assets (`companion-assets` job), so `provision/install-companions.sh` can
download prebuilt binaries instead of cloning the Flutter SDK on every
install. That job takes a few extra minutes after the tag push -- installs
that happen to land in that window just fall back to building from source,
same as before this existed, so there's nothing to wait for or babysit.

Installs on the `release` channel (the default) pick this up the next time
`install.ps1` or `provision/update.sh --channel release` runs, by querying
`GET /repos/m0nnnna/nekos/releases/latest` and checking out that tag. If the
auto-generated notes miss something worth calling out by hand (new baseline
apps, install.ps1 behavior changes, known gaps from FEATURES.md's "Known
gaps" section), edit the published release afterward with
`gh release edit vX.Y.Z --notes "..."`.
