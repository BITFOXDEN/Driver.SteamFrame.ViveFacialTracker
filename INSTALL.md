# Installing vftstream on your Steam Frame

A step-by-step walkthrough for getting the VIVE Facial Tracker streaming to
Baballonia on your PC. It should take about five minutes.

There are two parts: a short one-time setup **on the headset**, and an
automated install **from your PC**. Only the headset part needs you to touch
the headset.

## What you need

- A Steam Frame with the VIVE Facial Tracker plugged into its rear USB port.
- A PC on the same Wi-Fi network as the headset.
- On Windows 10/11, the built-in **OpenSSH Client** (Settings → Apps →
  Optional features → OpenSSH Client) — then just run `install.ps1` in
  PowerShell. On Linux, macOS, WSL, or Git-Bash, run `install.sh` instead.
- The headset already ships with `gcc`, `make` and `libjpeg-turbo` — no root
  changes are required.

## Step 1 — Enable Developer Mode and set a password (headset)

The headset only accepts SSH once Developer Mode is on and the `steamos` user
has a password.

1. On the headset, press the **Steam button** and open **Settings**.
2. Open the **System** tab and turn on **Developer Mode**.
3. Open the **Developer** tab and choose **Set User Password**, then set a
   password for the `steamos` user. Remember it — you will type it during the
   install.
4. Confirm the headset and your PC are on the same network.

## Step 2 — Find the headset's address (PC)

Try `steamos@frame` first — many networks resolve the headset's mDNS name. If
that does not work from your PC, use the headset's IP address instead. On the
headset, run `ip addr` to find it, or check your router.

## Step 3 — Run the installer (PC)

From the folder you downloaded this project into.

**Windows (PowerShell):**

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

(The `-ExecutionPolicy Bypass` flag is only needed if Windows blocks local
scripts; if `.\install.ps1` already runs, that is fine too.)

**Linux, macOS, WSL, or Git-Bash:**

```bash
bash install.sh
```

Both installers:

1. Explain the headset setup above and wait for you to confirm.
2. Ask for the headset address (press Enter for `steamos@frame`).
3. Copy the source to the headset, build it there, and install the service.
4. Print the Baballonia stream URL and a live camera status.

You will be asked for the `steamos` password once or twice; type the one you set
in Step 1. When it finishes you will see `all done.`

### Non-interactive / advanced

```powershell
# Windows
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Headset steamos@192.168.1.42
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Yes
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall
```

```bash
# Linux / macOS / WSL / Git-Bash
bash install.sh --host steamos@192.168.1.42   # skip auto-detection
bash install.sh -y                            # no confirmation prompts
bash install.sh --uninstall                   # stop and remove everything
```

## Step 4 — Connect from Baballonia (PC)

In Baballonia: **Home** tab → add camera → provider **Wireless/IP Camera** →
enter:

```
http://<headset>:8097/stream
```

The mono grayscale image should appear within a few seconds and feed the normal
Babble pipeline (inference, VRChat / Resonite / CVR output).

## Managing the service on the headset

```bash
# status
ssh steamos@frame 'export XDG_RUNTIME_DIR=/run/user/1000; systemctl --user status vftstream'
# logs (live)
ssh steamos@frame 'export XDG_RUNTIME_DIR=/run/user/1000; journalctl --user -u vftstream -f'
# restart
ssh steamos@frame 'export XDG_RUNTIME_DIR=/run/user/1000; systemctl --user restart vftstream'
```

The service starts automatically at boot and after sleep/wake.

## If something goes wrong

| Problem | Fix |
| ------- | --- |
| Password prompt never appears / connection refused | Developer Mode is off or no password is set — redo Step 1. |
| `Could not resolve hostname frame` | Use the headset's IP instead of `frame` (Step 2). |
| `libjpeg-turbo headers not found` | On the headset: `sudo pacman -S libjpeg-turbo`, then re-run the installer. |
| `systemd user bus is not reachable` | Log in to the headset (so the `steamos` session is running), then re-run. |
| Installer keeps looping | Check the Wi-Fi network and that the tracker is plugged in; the exact error is printed each attempt. |
| `install.ps1 cannot be loaded because running scripts is disabled` | Run it as `powershell -ExecutionPolicy Bypass -File .\install.ps1`. |
| Service runs but the image is black | IR brownout on weak power — see the Troubleshooting section of [README.md](README.md). |

## Prefer to do it by hand?

The manual build-and-deploy steps are in the **Build & deploy** section of
[README.md](README.md).
