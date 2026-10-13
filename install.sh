#!/usr/bin/env bash
# install.sh - guided one-command installer for vftstream on the Steam Frame.
#
# Run this from your PC (Linux, macOS, WSL, or Git-Bash on Windows) with the
# headset on the same network. It stages the source over SSH, builds it on the
# headset, and installs/restarts the user-level systemd service.
#
# Usage:
#   bash install.sh                 # guided install
#   bash install.sh --host steamos@192.168.1.42
#   bash install.sh --uninstall
#   bash install.sh -y              # no confirmation prompts
#
# See INSTALL.md for the full walkthrough, including the headset-side
# Developer Mode / password setup that only you can do.

set -euo pipefail

HOST_DEFAULT="steamos@frame"
PORT="8097"
HOST=""
ASSUME_YES=0
UNINSTALL=0

usage() {
    cat <<'EOF'
vftstream installer for the Steam Frame

Usage:
  bash install.sh [options]

Options:
  --host USER@HOST   headset SSH target (default: steamos@frame)
  --uninstall        stop and remove the service and installed source
  -y, --yes          assume "yes" for confirmations
  -h, --help         show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs a value}"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -y|--yes) ASSUME_YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; echo; echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------------------
# presentation helpers
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    BOLD=$'\033[1m'; RED=$'\033[31m'
    GRN=$'\033[32m'; YLW=$'\033[33m'; RST=$'\033[0m'
else
    BOLD=""; RED=""; GRN=""; YLW=""; RST=""
fi
step() { printf '\n%s==> %s%s\n' "$BOLD" "$*" "$RST"; }
ok()   { printf '%s  ok %s %s\n' "$GRN" "$RST" "$*"; }
warn() { printf '%s  !! %s %s\n' "$YLW" "$RST" "$*"; }
die()  { printf '%serror:%s %s\n' "$RED" "$RST" "$*" >&2; exit 1; }

confirm() { # prompt, default (y|n)
    local msg=$1 def=${2:-y} ans h
    if [ "$ASSUME_YES" -eq 1 ]; then
        return 0
    fi
    if [ "$def" = y ]; then h="Y/n"; else h="y/N"; fi
    read -r -p "$msg [$h] " ans </dev/tty || true
    ans=${ans:-$def}
    case "$ans" in
        [Yy]*) return 0 ;;
        *)     return 1 ;;
    esac
}

prompt_host() { # always prompts, current HOST as the default
    local def="${HOST:-$HOST_DEFAULT}" h
    read -r -p "Headset SSH target (user@host or user@IP) [$def]: " h </dev/tty || true
    HOST=${h:-$def}
}

# ---------------------------------------------------------------------------
# sanity checks + staging tarball
# ---------------------------------------------------------------------------
for t in ssh scp tar; do
    command -v "$t" >/dev/null 2>&1 || die "'$t' is not installed or not on PATH."
done

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
[ -f "$SRC_DIR/src/main.cpp" ] || die "run this script from the vftstream repo root."

TMP="$(mktemp -d "${TMPDIR:-/tmp}/vftstream.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
tar czf "$TMP/vftstream-src.tar.gz" -C "$SRC_DIR" src Makefile

SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)
SCP_OPTS=("${SSH_OPTS[@]}")

# ---------------------------------------------------------------------------
# remote payloads (run on the headset)
# ---------------------------------------------------------------------------
REMOTE_UNINSTALL=$(cat <<'REMOTE'
set -e
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
systemctl --user disable --now vftstream 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/vftstream.service"
systemctl --user daemon-reload 2>/dev/null || true
rm -rf "$HOME/vftstream-src"
echo "vftstream removed."
REMOTE
)

REMOTE_INSTALL=$(cat <<'REMOTE'
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
REMOTE
)

# ---------------------------------------------------------------------------
# step 1 - headset preparation (only the user can do this part)
# ---------------------------------------------------------------------------
if [ "$UNINSTALL" -eq 0 ]; then
    step "Step 1 - prepare the headset (one time)"
    cat <<'GUIDE'
The headset needs Developer Mode and a user password before it will accept SSH.
On the headset:

  1. Press the Steam button and open  Settings.
  2. Go to  System  and turn on  Developer Mode.
  3. Go to  Developer  and choose  Set User Password, then set a password for
     the "steamos" user. Remember it - you will type it during the install.
  4. Make sure the headset and this PC are on the same network.

This is the only part that cannot be automated.
GUIDE
    confirm "Done - continue with the install?" y || die "aborted by user."
fi

# ---------------------------------------------------------------------------
# step 2 - headset address
# ---------------------------------------------------------------------------
step "Step 2 - find the headset"
cat <<'GUIDE'
The default target "steamos@frame" uses the headset's mDNS name. If your PC
cannot resolve it, enter the headset's IP address instead (on the headset run
"ip addr" to find it, or check your router).
GUIDE
if [ "$ASSUME_YES" -eq 0 ]; then
    prompt_host
else
    HOST="${HOST:-$HOST_DEFAULT}"
fi

# ---------------------------------------------------------------------------
# step 3 - deploy (with retries so the same command works first try)
# ---------------------------------------------------------------------------
HOSTONLY="${HOST#*@}"
[ -n "$HOSTONLY" ] || die "invalid host '$HOST'."

attempt=0
while :; do
    attempt=$((attempt + 1))
    if [ "$UNINSTALL" -eq 1 ]; then
        step "Uninstalling from $HOST"
        if ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_UNINSTALL"; then
            ok "vftstream removed from $HOST."
            exit 0
        fi
    else
        step "Installing on $HOST"
        if scp "${SCP_OPTS[@]}" "$TMP/vftstream-src.tar.gz" \
               "$HOST:/tmp/vftstream-src.tar.gz" \
           && ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_INSTALL"; then
            ok "installed and service started on $HOST."
            break
        fi
    fi

    warn "could not finish on $HOST."
    cat <<'RETRY'
Check all of these, then retry:
  - Developer Mode is ON and a password is set (Settings > Developer).
  - The headset is powered on, awake, and on the same network as this PC.
  - The host is reachable: the password prompt should appear at least once.
  - If "steamos@frame" does not resolve, use the headset's IP address.
RETRY
    if [ "$ASSUME_YES" -eq 1 ] || [ "$attempt" -ge 5 ]; then
        die "giving up after $attempt attempt(s)."
    fi
    confirm "Retry?" y || die "aborted."
    prompt_host
done
# ---------------------------------------------------------------------------
# step 4 - connect from Baballonia
# ---------------------------------------------------------------------------
step "Step 4 - connect from Baballonia (on this PC)"
cat <<GUIDE
In Baballonia:
  Home tab -> add camera -> provider  "Wireless/IP Camera"
  Stream URL:  http://$HOSTONLY:$PORT/stream

The first frame may take a few seconds while the tracker wakes up. The service
restarts automatically and survives sleep/wake and reboots. Logs:

  ssh $HOST 'export XDG_RUNTIME_DIR=/run/user/1000; journalctl --user -u vftstream -f'
GUIDE

if command -v curl >/dev/null 2>&1; then
    printf '\nWaiting for the camera to report status'
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        out="$(curl -s --max-time 3 "http://$HOSTONLY:$PORT/status" || true)"
        if [ -n "$out" ]; then
            printf '\n'; ok "camera: $out"
            break
        fi
        printf '.'
        sleep 2
    done
    printf '\n'
fi

ok "all done."
