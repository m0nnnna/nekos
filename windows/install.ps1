# Installs nekOS: a custom desktop environment (Void Linux) that runs
# nested inside WSL2, forwarded to a native Windows window via WSLg.
#
# Safe to re-run: if nekos-void already exists, this skips straight to
# updating/rebuilding nekOS + its companion apps, so it also doubles as the
# update path. See README.md for requirements and manual uninstall.
#
# Run from an ordinary (non-admin) PowerShell prompt:
#   ./install.ps1
#   ./install.ps1 -Channel master   # track the dev branch instead of releases
#
# -Channel is remembered across re-runs (see provision/update.sh) -- you only
# need to pass it again to switch channels.

param(
    [ValidateSet("release", "master")]
    [string]$Channel = "release"
)

$ErrorActionPreference = "Stop"

$DistroName = "nekos-void"
$InstallRoot = Join-Path $env:LOCALAPPDATA "nekOS"
$WslTargetDir = Join-Path $InstallRoot "wsl"
$NekosRepoUrl = "https://github.com/m0nnnna/nekos.git"

function Write-Step($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Invoke-InDistro($ShellCommand) {
    wsl.exe -d $DistroName -- sh -c $ShellCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed inside ${DistroName}: $ShellCommand"
    }
}

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null

# --- 1. WSL2 present? -------------------------------------------------------
Write-Step "Checking for WSL..."
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Write-Step "WSL isn't installed -- installing it now (this usually requires a reboot)..."
    wsl.exe --install --no-distribution
    Write-Host ""
    Write-Host "WSL was just installed. Please reboot Windows, then re-run this script." -ForegroundColor Yellow
    exit 0
}
wsl.exe --set-default-version 2 | Out-Null

# --- 2. Import the nekos-void distro (skip if it already exists) -----------
$existingDistros = (wsl.exe -l -q) -replace "`0", "" | Where-Object { $_.Trim() -ne "" }
if ($existingDistros -contains $DistroName) {
    Write-Step "$DistroName already exists -- skipping import, updating instead."
} else {
    Write-Step "Finding the latest official Void Linux rootfs..."
    $listing = Invoke-WebRequest -Uri "https://repo-default.voidlinux.org/live/current/" -UseBasicParsing
    $rootfsName = [regex]::Matches($listing.Content, 'void-x86_64-ROOTFS-\d+\.tar\.xz') |
        Select-Object -Last 1 -ExpandProperty Value
    if (-not $rootfsName) {
        throw "Couldn't find a void-x86_64-ROOTFS-*.tar.xz tarball at repo-default.voidlinux.org/live/current/."
    }
    $rootfsUrl = "https://repo-default.voidlinux.org/live/current/$rootfsName"
    $rootfsPath = Join-Path $InstallRoot $rootfsName

    if (-not (Test-Path $rootfsPath)) {
        Write-Step "Downloading $rootfsName (~50 MB)..."
        Invoke-WebRequest -Uri $rootfsUrl -OutFile $rootfsPath
    }

    Write-Step "Importing $DistroName into WSL..."
    New-Item -ItemType Directory -Force -Path $WslTargetDir | Out-Null
    wsl.exe --import $DistroName $WslTargetDir $rootfsPath --version 2
    if ($LASTEXITCODE -ne 0) {
        throw "wsl --import failed. Make sure virtualization is enabled (BIOS + " + `
              "'Virtual Machine Platform' Windows feature) and try again."
    }

    Write-Step "Bootstrapping the base Void Linux system (rootfs tarballs ship without one)..."
    Invoke-InDistro "xbps-install -Sy base-system git"
}

# Always fully upgrade the installed package set before installing/refreshing
# anything else -- not just on a fresh import. `xbps-install -Sy <pkglist>`
# (setup-void.sh, below) only refreshes the repodata index, it does not
# upgrade already-installed packages; on a distro that's sat untouched for a
# while, that drift between "index" and "installed base" is exactly what
# produces xbps's "unresolved shlibs" error the first time setup-void.sh
# tries to pull in a package that needs a newer soname than the stale base
# system provides. Void's own docs call for running -Suy twice: the first
# pass can update xbps itself mid-transaction.
Write-Step "Fully upgrading $DistroName's package set..."
Invoke-InDistro "xbps-install -Suy xbps"
Invoke-InDistro "xbps-install -Suy"

# --- 3. Clone nekOS (full clone -- update.sh needs release tags reachable,
#        which a shallow clone can't cleanly check out later) ---------------
Write-Step "Cloning nekOS..."
Invoke-InDistro "xbps-install -Sy git"
Invoke-InDistro "cd ~ && [ -d nekos/.git ] || git clone $NekosRepoUrl nekos"

Write-Step "Installing the nekOS baseline package set (chromium, mpv, dev toolchain, etc.)..."
Invoke-InDistro "cd ~/nekos && sh provision/setup-void.sh"

Write-Step "Updating nekOS to the latest $Channel and building it..."
Invoke-InDistro "sh ~/nekos/provision/update.sh --channel $Channel"

Write-Step "Installing companion apps (NekoShot, NekoPlayer) -- this is the slow part, mostly the Flutter SDK build..."
Invoke-InDistro "cd ~/nekos && sh provision/install-companions.sh"

# --- 4. Windows-side launcher + shortcut ------------------------------------
Write-Step "Setting up the Windows-side launcher..."
$launchSrc = (wsl.exe -d $DistroName -- wslpath -w "~/nekos/windows/launch.ps1").Trim()
$launchDst = Join-Path $InstallRoot "launch.ps1"
Copy-Item -Path $launchSrc -Destination $launchDst -Force

$startMenu = [Environment]::GetFolderPath("StartMenu")
$shortcutPath = Join-Path $startMenu "nekOS.lnk"
$wshell = New-Object -ComObject WScript.Shell
$shortcut = $wshell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = "powershell.exe"
$shortcut.Arguments = "-NoLogo -ExecutionPolicy Bypass -File `"$launchDst`""
$shortcut.WorkingDirectory = $InstallRoot
$shortcut.Description = "Launch nekOS"
$shortcut.Save()

Write-Step "Done!"
Write-Host "Launch nekOS from the Start Menu, or run: $launchDst"
Write-Host "Re-run this script any time to update nekOS and its companion apps."
