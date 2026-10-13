# vftstream

A small native daemon for the **Steam Frame (SteamOS, ARM64)** that captures the
IR camera of a locally plugged-in **VIVE Facial Tracker** and serves it as a live
**MJPEG over HTTP** stream. It is designed to feed
[Baballonia](https://github.com/Project-Babble/Baballonia)'s **Wireless/IP
Camera** provider running on a gaming PC, which then drives VRChat / Resonite /
CVR over OSC.

```
┌──────────────────────────────┐            ┌─────────────────────────────┐
│ Steam Frame (SteamOS, ARM64) │            │ Gaming PC                   │
│                              │   Wi-Fi    │ ┌─────────────────────────┐ │
│  [VIVE Facial Tracker]       │ ────────►  │ │ Baballonia              │ │
│      │ USB2 (host)           │  MJPEG     │ │   provider:             │ │
│  ┌───▼───────────────────┐   │  400x400   │ │   "Wireless/IP Camera"  │ │
│  │ vftstream daemon      │   │  ~30-60fps │ │   http://frame:8097/    │ │
│  │  usbfs + libjpeg-turbo│   │            │ └─────────────────────────┘ │
│  └───────────────────────┘   │            │      │ OSC (localhost)      │
└──────────────────────────────┘            │      ▼                      │
                                            │   VRChat / Resonite / CVR   │
                                            └─────────────────────────────┘
```

Inference stays on the PC's GPU (DirectML) and OSC to the game stays on
localhost; the only Wi-Fi leg is the ~2–3 Mbps MJPEG stream over plain TCP.

## How it works

The Steam Frame kernel ships **no `uvcvideo`** module, so the daemon talks to
the tracker directly through Linux **usbfs** (`/dev/bus/usb/BBB/DDD`,
autodetected via sysfs VID `0bb4:0321`):

1. **Port reset first.** The tracker's extension-unit state machine keeps stale
   state across sessions and will ACK the handshake while the sensors idle in
   the factory test-bar mode. A `USBDEVFS_RESET` before the handshake revives a
   device with a dead pipe. It is skipped behind a USB hub, where a port reset
   would drop the device off the bus.
2. **Extension-unit handshake** (unit `4`, selector `2`) at the persona's
   native scratch size, with a `GET_LEN → SET_CUR → GET_LEN → GET_CUR` rhythm
   around every command and a three-magic cadence:
   `magic → stream-off → magic → 11 sensor registers → magic → stream-on`.
   The registers mirror SRanipal (`00=40, 08=01, 70=00, 02/03/04=IR exposure,
   0e=00, 05/06/07=b2, 0f=03`).
3. **UVC PROBE/COMMIT + `SET_INTERFACE(1, alt 0)`** on the streaming interface
   after stream-on. On a normal host `uvcvideo` does this in-kernel; on the
   High-Speed persona it is what actually arms the streaming interface.
4. **Bulk capture** via asynchronous usbfs URBs (8 × 16 KB) reaped by a
   dedicated pump thread. The wire is a raw byte stream: 12-byte UVC payload
   headers every 32768 bytes, frames EOF-delimited at **400×400 YUYV @ 60 fps**
   (320000 payload bytes). The parser pins the chunk stride and validates frame
   starts, since image data false-positives as headers otherwise.
5. The daemon discards the chroma (the image is grayscale in **Y**), crops the
   left camera (columns 0–200), upscales to 400×400, applies the **5×5
   gaussian + tone LUT** matching Babble's `VFTCommon`, encodes to grayscale
   JPEG with libjpeg-turbo, and serves it over HTTP.

The protocol is cross-checked against DragonLord's
[vivefacialtracker](https://github.com/LordOfDragons/vivefacialtracker),
Baballonia's `LinuxUsbCommunicator`, and USBPcap captures of SRanipal on
Windows.

## Requirements

- Steam Frame with Developer Mode enabled.
- `gcc`, `make` and `libjpeg-turbo` — all present on the headset by default;
  no root partition changes are needed. (On other distros:
  `sudo pacman -S libjpeg-turbo`.)

## Build & deploy

**Easy path:** run the guided installer from your PC — it walks through the
one-time headset setup (Developer Mode + password) and does the SSH build and
service install for you. See [INSTALL.md](INSTALL.md).

```powershell
# Windows (PowerShell)
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

```bash
# Linux, macOS, WSL, or Git-Bash
bash install.sh
```

**Manual path.** From a machine with `ssh` + `scp` (on Windows use
`pscp`/`plink`, or run `scripts/build.remote.sh` under WSL/Git-Bash):

```bash
ssh steamos@frame 'mkdir -p ~/vftstream-src'
scp -r src Makefile steamos@frame:~/vftstream-src/
ssh steamos@frame 'cd ~/vftstream-src && make'
```

Install as a **user-level systemd service**. (The system paths `/usr/local` and
`/etc` live on SteamOS's read-only partition, so a user service avoids that
entirely and survives sleep/wake and reboots.)

```bash
ssh steamos@frame 'mkdir -p ~/.config/systemd/user'
cat > unit <<'EOF' | ssh steamos@frame 'cat > ~/.config/systemd/user/vftstream.service'
[Unit]
Description=vftstream - VIVE Facial Tracker MJPEG stream for Baballonia
After=multi-user.target

[Service]
ExecStart=/home/steamos/vftstream-src/vftstream
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
EOF
ssh steamos@frame 'export XDG_RUNTIME_DIR=/run/user/1000; \
    systemctl --user daemon-reload && systemctl --user enable --now vftstream'
```

`packaging/vftstream.service` provides an equivalent system unit for hosts with
a writable root.

Over SSH, `systemctl --user` needs `XDG_RUNTIME_DIR=/run/user/1000`; from a
terminal on the headset it works directly. Watch the logs with:

```bash
ssh steamos@frame 'export XDG_RUNTIME_DIR=/run/user/1000; journalctl --user -u vftstream -f'
```

Sanity checks:

```bash
ssh steamos@frame 'lsusb | grep -i HTC'        # tracker enumerated?
ssh steamos@frame 'curl -s localhost:8097/status'
# -> {"device":"/dev/bus/usb/001/002","fps":27.18,"encode_ms":2.19,"frames":2700,"reconnects":0}
```

## Using it from Baballonia

In Baballonia: **Home** tab → add camera → provider **Wireless/IP Camera** →
`http://frame:8097/stream` (mDNS name from the PC, or the headset's IP). Press
connect; the mono grayscale image appears and the standard Babble pipeline
(inference, OSC output) takes over.

## Endpoints

| Endpoint   | Description                                      |
| ---------- | ------------------------------------------------ |
| `/stream`  | MJPEG `multipart/x-mixed-replace` (for Baballonia) |
| `/snap`    | Single JPEG                                       |
| `/status`  | JSON: fps, encode_ms, frames, reconnects           |
| `/metrics` | Plain-text counters                                |

A set of `/debug/*` routes exposes low-level XU and endpoint control for
bring-up and diagnostics.

## Configuration

CLI flags (see `--help`), or an ini file via `--ini ~/vftstream.ini`. The
installed service passes CLI flags in `ExecStart`.

| Option   | CLI          | Default | Meaning                                  |
| -------- | ------------ | ------- | ---------------------------------------- |
| port     | `--port`     | 8097    | HTTP listen port                         |
| device   | `--device`   | (auto)  | usbfs node; empty = autodetect           |
| quality  | `--quality`  | 85      | JPEG quality 1–100                       |
| fps      | `--fps`      | 60      | Requested capture fps (max 60)           |
| ir_level | `--ir-level` | 255     | IR LED / exposure registers 02–04        |
| lut      | `--no-lut`   | on      | Babble gamma LUT                         |
| blur     | `--no-blur`  | on      | 5×5 gaussian                             |
| hold     | ini: `hold`  | off     | Hold the XU session open (diagnostics)   |

## Performance

The hot path is integer-only and builds on-device with
`-O3 -mcpu=native -ffast-math -flto`. At 60 fps the per-frame budget is
16.6 ms; process + encode runs in roughly 1–1.5 ms per frame. The crop is
fused into the resize's source sampling, the 5×5 gaussian is u16 fixed-point,
the tone LUT is folded into the blur's final store, and frame hand-offs are
zero-copy, so the steady state does not touch the allocator.

## USB notes for the Frame

- The tracker attaches to the headset's rear port, which is **USB 2.0-only**,
  so it always runs its High-Speed persona there.
- The port is dual-role: if the headset is **charged through it**, the port sits
  in device/sink mode and the tracker cannot enumerate. Power the headset
  normally and leave the port to the tracker.
- The tracker is bus-powered (≤512 mA). On weak VBUS the IR LED at maximum
  (`ir_level=255`) can brown the camera out; the daemon auto-recovers, but lower
  `--ir-level 128` if it happens often.

## Troubleshooting

| Symptom | Remedy |
| ------- | ------ |
| `no HTC camera on the bus yet` | Tracker absent: replug it. If `dmesg` shows `device not accepting address, error -71`, the tracker is in a deep brownout — restart the Frame, or leave it unplugged 5–10 min (SRanipal on a PC also revives it). The daemon picks it up automatically. |
| Stream black, then `reconnects` climbing | IR brownout at `ir_level=255` on marginal VBUS. It self-recovers; run with `--ir-level 128` if frequent. |
| `readFrame: pipe dead; HARD reopen` loop | The device stopped mid-stream; the daemon resets once per cycle. If it loops forever, confirm the tracker is still in `lsusb`. |
| `readFrame: parser starved, SOFT reopen` loop | Wire busy but no complete frames — stale device state. A restart of the Frame clears it if it persists. |
| `frame` hostname fails | Use the headset's IPv4 (`ip addr` over ssh). |
| Garbage colors in a viewer | Expected: the raw stream is fake-YUV grayscale; the daemon always outputs pure grayscale JPEG. |
| Port seems dead after headset sleep | The port flipped to device/sink mode. Unplug whatever powers it; the port returns to host mode and the service reconnects on its own. |

## Source layout

```
src/
  main.cpp               capture -> encode -> publish loop, endpoints, reconnect policy
  usb_uvc.{hpp,cpp}      usbfs backend: XU handshake, VS negotiation,
                         bulk URB pump thread, UVC payload-stream parser
  pipeline.{hpp,cpp}     fused crop/resize (cached maps) + fixed-point
                         5x5 gaussian + folded LUT (Babble-accurate)
  encoder.hpp            turbojpeg grayscale encoder
  frame_topic.hpp        latest-frame mailbox with zero-copy pooled publish
  http_server.{hpp,cpp}  threaded HTTP server (single-writev multipart sends)
  config.{hpp,cpp}       ini + CLI
Makefile                 on-device build (-O3 -mcpu=native -ffast-math -flto)
tests/                   golden pipeline test, FrameTopic and HTTP/MJPEG
                         end-to-end tests, benchmark;
                         run with tests/build_and_run.sh
packaging/               systemd unit template
scripts/                 build-on-headset helper
install.sh               guided installer (Linux/macOS/WSL/Git-Bash)
install.ps1              guided installer (Windows PowerShell)
INSTALL.md               step-by-step installation guide
```

## Credits & license

- VFT extension-unit protocol and register map: DragonLord's
  [vivefacialtracker](https://github.com/LordOfDragons/vivefacialtracker) (MIT)
  and the Project-Babble team (Apache-2.0), who documented and implemented it.
- Image-processing constants match Baballonia's `VFTCommon.cs` so frames are
  model-compatible.
- MJPEG structure follows Baballonia's `IpCameraCapture` parsing.

This project is licensed under the GNU General Public License v3.0 — see
`LICENSE`.
