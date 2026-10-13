#Requires -Version 5.1
<#
.SYNOPSIS
    Guided installer for vftstream on the Steam Frame (Windows).

.DESCRIPTION
    Run this from your PC with the headset on the same network. It copies the
    source to the headset over SSH, builds it there, and installs / restarts the
    user-level systemd service.

    Developer Mode and the "steamos" user password must be set on the headset
    first - see INSTALL.md. That part cannot be automated.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Headset steamos@192.168.1.42
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Yes
#>
[CmdletBinding()]
param(
    [string]$Headset = "",
    [switch]$Uninstall,
    [switch]$Yes,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$DefaultHost = "steamos@frame"
$Port = "8097"

function Show-Usage {
    Write-Host @"
vftstream installer for the Steam Frame

Usage:
  powershell -ExecutionPolicy Bypass -File .\install.ps1 [options]

Options:
  -Headset USER@HOST   headset SSH target (default: steamos@frame)
  -Uninstall           stop and remove the service and installed source
  -Yes                 assume "yes" for confirmations
  -Help                show this help

Note: the ExecutionPolicy flag is only needed if Windows blocks local scripts.
"@
}

function Write-Step([string]$m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "  ok $m" -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host "  !! $m" -ForegroundColor Yellow }
function Die([string]$m)        { Write-Host "error: $m" -ForegroundColor Red; exit 1 }

function Confirm-Step([string]$Question, [string]$Default = "Y") {
    if ($Yes) { return $true }
    $hint = if ($Default -eq "Y") { "Y/n" } else { "y/N" }
    $ans = Read-Host "$Question [$hint]"
    if ([string]::IsNullOrWhiteSpace($ans)) { $ans = $Default }
    return ($ans -match '^[Yy]')
}

function Read-HeadsedHost([string]$Default) {
    if ($Yes) { return $Default }
    $ans = Read-Host "Headset SSH target (user@host or user@IP) [$Default]"
    if ([string]::IsNullOrWhiteSpace($ans)) { return $Default }
    return $ans
}

function Write-RemoteScript([string]$Path, [string]$Text) {
    $lf = ($Text -replace "`r`n", "`n")
    [System.IO.File]::WriteAllText($Path, $lf, (New-Object System.Text.UTF8Encoding($false)))
}

if ($Help) { Show-Usage; exit 0 }

# ---------------------------------------------------------------------------
# local prerequisites
# ---------------------------------------------------------------------------
foreach ($tool in @("ssh", "scp", "tar")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        if ($tool -eq "tar") {
            Die "'tar' not found. It ships with Windows 10 (1803+)."
        }
        Die "'$tool' not found. Install the OpenSSH Client optional feature (Settings > Apps > Optional features)."
    }
}

$SrcDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not (Test-Path (Join-Path $SrcDir "src\main.cpp"))) {
    Die "run this script from the vftstream repo root."
}

$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("vftstream-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Tmp | Out-Null

# ---------------------------------------------------------------------------
# remote payloads (run on the headset with bash)
# ---------------------------------------------------------------------------
$remoteInstall = @'
set -e
SRC="$HOME/vftstream-src"

echo "==> headset: $(uname -m)"
rm -rf "$SRC"
mkdir -p "$SRC"
tar xzf /tmp/vftstream-src.tar.gz -C "$SRC"
rm -f /tmp/vftstream-src.tar.gz
cd "$SRC"

for t in gcc make; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "ERROR: '$t' is not installed on the headset." >&2
        exit 3
    fi
done
if ! printf '#include <turbojpeg.h>\n' | gcc -x c -fsyntax-only - >/dev/null 2>&1; then
    echo "ERROR: libjpeg-turbo headers not found on the headset." >&2
    echo "Install them there with:  sudo pacman -S libjpeg-turbo" >&2
    exit 4
fi

echo "==> compiling"
make clean >/dev/null 2>&1 || true
make -j"$(nproc)"

echo "==> installing the user service"
mkdir -p "$HOME/.config/systemd/user"
cat > "$HOME/.config/systemd/user/vftstream.service" <<'UNIT'
[Unit]
Description=vftstream - VIVE Facial Tracker MJPEG stream for Baballonia
After=multi-user.target

[Service]
ExecStart=%h/vftstream-src/vftstream
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNIT

export XDG_RUNTIME_DIR="/run/user/$(id -u)"
if ! systemctl --user daemon-reload 2>/dev/null; then
    echo "ERROR: the systemd user bus is not reachable over SSH." >&2
    echo "Make sure the steamos user session is running on the headset," >&2
    echo "then run the installer again (or use the on-headset terminal)." >&2
    exit 5
fi
systemctl --user enable --now vftstream

echo "==> service state"
systemctl --user --no-pager --full status vftstream 2>/dev/null | head -n 6 || true
if command -v curl >/dev/null 2>&1; then
    printf '==> camera status: '
    curl -s --max-time 3 http://localhost:8097/status || echo "(starting; may take a few seconds)"
    echo
fi
echo "install complete."
'@

$remoteUninstall = @'
set -e
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
systemctl --user disable --now vftstream 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/vftstream.service"
systemctl --user daemon-reload 2>/dev/null || true
rm -rf "$HOME/vftstream-src"
echo "vftstream removed."
'@

try {
    $tarball = Join-Path $Tmp "vftstream-src.tar.gz"
    $installScriptPath = Join-Path $Tmp "vftstream-install.sh"
    $uninstallScriptPath = Join-Path $Tmp "vftstream-uninstall.sh"
    Write-RemoteScript $installScriptPath $remoteInstall
    Write-RemoteScript $uninstallScriptPath $remoteUninstall

    Write-Step "Staging source"
    & tar -czf $tarball -C $SrcDir src Makefile
    if ($LASTEXITCODE -ne 0) { Die "failed to create the source archive." }

    $sshOpts = @("-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=15")

    # -----------------------------------------------------------------------
    # step 1 - headset preparation (only the user can do this part)
    # -----------------------------------------------------------------------
    if (-not $Uninstall) {
        Write-Step "Step 1 - prepare the headset (one time)"
        Write-Host @"
The headset needs Developer Mode and a user password before it will accept SSH.
On the headset:

  1. Press the Steam button and open  Settings.
  2. Go to  System  and turn on  Developer Mode.
  3. Go to  Developer  and choose  Set User Password, then set a password for
     the "steamos" user. Remember it - you will type it during the install.
  4. Make sure the headset and this PC are on the same network.

This is the only part that cannot be automated.
"@
        if (-not (Confirm-Step "Done - continue with the install?" "Y")) {
            Die "aborted by user."
        }
    }

    # -----------------------------------------------------------------------
    # step 2 - headset address
    # -----------------------------------------------------------------------
    Write-Step "Step 2 - find the headset"
    Write-Host @"
The default target "steamos@frame" uses the headset's mDNS name. If your PC
cannot resolve it, enter the headset's IP address instead (on the headset run
"ip addr" to find it, or check your router).
"@
    if (-not $Headset) { $Headset = $DefaultHost }
    if (-not $Yes) { $Headset = Read-HeadsedHost $Headset }

    $hostOnly = ($Headset -split '@')[-1]
    if ([string]::IsNullOrWhiteSpace($hostOnly)) { Die "invalid host '$Headset'." }

    # -----------------------------------------------------------------------
    # step 3 - deploy (retries so the same command works first try)
    # -----------------------------------------------------------------------
    $attempt = 0
    $installed = $false
    while (-not $installed) {
        $attempt++
        if ($Uninstall) {
            Write-Step "Uninstalling from $Headset"
            & scp @sshOpts $uninstallScriptPath "${Headset}:/tmp/"
            if ($LASTEXITCODE -eq 0) {
                & ssh @sshOpts $Headset "bash /tmp/vftstream-uninstall.sh"
                if ($LASTEXITCODE -eq 0) {
                    Write-Ok "vftstream removed from $Headset."
                    exit 0
                }
            }
        } else {
            Write-Step "Installing on $Headset"
            & scp @sshOpts $tarball $installScriptPath "${Headset}:/tmp/"
            if ($LASTEXITCODE -eq 0) {
                & ssh @sshOpts $Headset "bash /tmp/vftstream-install.sh"
                if ($LASTEXITCODE -eq 0) {
                    Write-Ok "installed and service started on $Headset."
                    $installed = $true
                    break
                }
            }
        }

        Write-Warn "could not finish on $Headset."
        Write-Host @"
Check all of these, then retry:
  - Developer Mode is ON and a password is set (Settings > Developer).
  - The headset is powered on, awake, and on the same network as this PC.
  - The host is reachable: the password prompt should appear at least once.
  - If "steamos@frame" does not resolve, use the headset's IP address.
"@
        if ($Yes -or $attempt -ge 5) { Die "giving up after $attempt attempt(s)." }
        if (-not (Confirm-Step "Retry?" "Y")) { Die "aborted." }
        $Headset = Read-HeadsedHost $Headset
        $hostOnly = ($Headset -split '@')[-1]
    }

    # -----------------------------------------------------------------------
    # step 4 - connect from Baballonia
    # -----------------------------------------------------------------------
    Write-Step "Step 4 - connect from Baballonia (on this PC)"
    Write-Host @"
In Baballonia:
  Home tab -> add camera -> provider  "Wireless/IP Camera"
  Stream URL:  http://${hostOnly}:$Port/stream

The first frame may take a few seconds while the tracker wakes up. The service
restarts automatically and survives sleep/wake and reboots. Logs:

  ssh $Headset 'export XDG_RUNTIME_DIR=/run/user/1000; journalctl --user -u vftstream -f'
"@

    Write-Host "`nWaiting for the camera to report status" -NoNewline
    for ($i = 0; $i -lt 10; $i++) {
        try {
            $status = (Invoke-RestMethod -Uri "http://${hostOnly}:$Port/status" -TimeoutSec 3 -UseBasicParsing)
            Write-Host ""
            Write-Ok ("camera: " + ($status | ConvertTo-Json -Compress))
            break
        } catch {
            Write-Host "." -NoNewline
            Start-Sleep -Seconds 2
        }
    }
    Write-Host ""

    Write-Ok "all done."
}
finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}
