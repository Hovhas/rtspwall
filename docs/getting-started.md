# Getting started

This tutorial takes you from a blank SD card to a camera wall on your TV. Plan for one evening. The steps from your first SSH login to the demo take about 5 minutes. Your first real camera takes about 15 minutes.

Every step ends with what you should see, so you always know whether to go on.

## What you need

- A Raspberry Pi 4 with its official power supply and a microSD card (16 GB or more).
- A micro-HDMI to HDMI cable and a TV or monitor. Either HDMI port works.
- An Ethernet cable. Wi-Fi works, but Ethernet is more reliable for video. If you use Wi-Fi, see [Wi-Fi power save](faq.md#does-wi-fi-work).
- IP cameras that can send an **H.264** stream. Pick the camera's [sub-stream](glossary.md#sub-stream) when you can.
- Another computer to flash the card and to log in over SSH.

Check [Will it work for me?](../README.md#will-it-work-for-me) first. A Pi 5, or cameras that only send H.265, will not work yet.

## Step 0: Flash Raspberry Pi OS Lite (about 10 minutes)

rtspwall needs the **Lite** image, not the Desktop image. A desktop keeps control of the screen, and only one program can drive it ([DRM master](glossary.md#drm-master)).

1. Install [Raspberry Pi Imager](https://www.raspberrypi.com/software/) on your computer.
2. Choose your device: **Raspberry Pi 4**.
3. Choose the OS: **Raspberry Pi OS (other)**, then **Raspberry Pi OS Lite (64-bit)**.
4. Choose your SD card.
5. When Imager offers OS customisation, open it and set a hostname, a user name and password, your Wi-Fi (if you use it), and **enable SSH**. The wording differs a little between Imager versions.
6. Write the card, put it in the Pi, connect Ethernet and the TV, and switch the TV on. Then power up the Pi.

**You should now see** the Pi's boot text on the TV, ending in a login prompt. Wait two minutes for the first boot to finish before you continue.

## Step 1: Log in over SSH (about 1 minute)

From a terminal on your computer. On macOS, Linux and Windows 10 or later (PowerShell) the command is the same:

```bash
ssh youruser@yourhostname.local
```

Use the user name and hostname you set in Step 0. If `.local` names do not work on your network, use the Pi's IP address from your router.

**You should now see** a prompt that ends in `$`, with your user and hostname in it.

## Step 2: Install rtspwall (about 2 to 4 minutes)

```bash
curl -fsSL https://github.com/Hovhas/rtspwall/releases/latest/download/get.sh | sudo bash
```

> **Testing the release candidate?** v0.1.0 is not out yet, so the command above does not work until it is. Install v0.1.0-rc2 with:
>
> ```bash
> curl -fsSL https://github.com/Hovhas/rtspwall/releases/download/v0.1.0-rc2/get.sh | sudo RTSPWALL_VERSION=v0.1.0-rc2 bash
> ```
>
> Reports are welcome through the [issue forms](https://github.com/Hovhas/rtspwall/issues/new/choose), especially the camera report.

The script refuses to continue on a Pi 5, a 32-bit system or an unsupported OS. It waits if another package job is running, checks the download against `SHA256SUMS` and installs the package for your OS. It never reboots, never starts the wall and never changes `gpu_mem`. The options `--yes` and `--no-gpu-mem` are accepted, but they do nothing.

If you prefer to read the script first, see [Other ways to install](../README.md#try-it-in-5-minutes).

**You should now see** the message `rtspwall is installed. It was not started and nothing was rebooted.` with the next steps, ending in `After adding cameras: sudo rtspwall doctor`. Run `doctor` once you have added your cameras (Step 6). If it shows a `FAIL` line about a desktop, see [Troubleshooting](troubleshooting.md#doctor-says-a-desktop-holds-the-display).

## Step 3: Run the demo (about 1 minute)

The demo shows four test tiles from clips that come with the package. It needs no cameras, so it proves that your HDMI cable, the decoder and the permissions work.

```bash
sudo rtspwall demo
```

**You should now see**, within about 10 seconds, a 2x2 wall of moving test pictures on the TV. The fourth tile switches between two clips every 8 seconds, with no black frame in between. The terminal prints `demo running: ...`. Press Ctrl-C to stop. The demo cleans up after itself.

If the screen stays black, run `sudo rtspwall doctor` and read the first `FAIL` line.

## Step 4: Probe your first camera (about 3 minutes)

First get the camera's RTSP URL. [Cameras](cameras.md) has the steps for each brand. Then check it:

```bash
sudo rtspwall probe
```

Paste the URL at the hidden prompt. Nothing is shown while you paste, and the URL never lands in your shell history. `probe` opens the stream, reads its codec, size and frame rate, and stops. It does not start playing. Do not put the URL on the command line: `probe` refuses a URL with a password, query string or token there. As root it reads the stream as the unprivileged user `rtspwall`, in a sandbox.

**You should now see** lines like `codec: H.264 ...`, `size:`, `fps:`, `decoder: ... % of the Pi 4 H.264 budget` and a `verdict:` of `PASS`. A `FAIL` comes with a `fix:` line. If it says H.265, switch the camera to H.264 ([Cameras](cameras.md#switching-a-camera-to-h264-or-a-sub-stream)).

## Step 5: Add the camera (about 3 minutes)

```bash
sudo rtspwall add front-door
```

Paste the URL again at the hidden prompt. `add` probes it, writes one line to `/etc/rtspwall/cameras.conf`, and starts the wall. It also enables the service, so the wall starts at every boot. If you run two `add` commands at once, the second waits for the first.

**You should now see** `added: front-door|...|1` and `enabled and started rtspwall`. Within about 15 seconds the camera appears in the top-left tile of the TV. The other three tiles stay black until you add more cameras.

## Step 6: Add more cameras (about 2 minutes each)

Repeat `sudo rtspwall add NAME` for each camera. Each one takes the next free cell of the 2x2 grid. To use a bigger grid, make cameras share a tile, or place tiles by hand, see [Configuration](configuration.md).

Saving a valid `/etc/rtspwall/cameras.conf` restarts the wall by itself. An invalid file leaves the running wall alone.

Now run the health check once. It tells you if you need `gpu_mem=256` and names any other problem, each with a fix:

```bash
sudo rtspwall doctor
```

If it says you need `gpu_mem=256`, run this, then reboot as a **separate** step:

```bash
sudo rtspwall doctor --fix
```

```bash
sudo reboot
```

`doctor --fix` asks first, makes a backup of `config.txt` and prints the command that undoes the change. It never reboots for you. If your `config.txt` sets `gpu_mem` under a section filter that `doctor` cannot evaluate, it changes nothing and tells you to edit the file by hand.

**You should now see** every camera you added in its own tile, and `sudo systemctl status rtspwall` showing `active (running)` with a status such as `N/N live`.

## Getting your console back

The wall owns the screen while it runs. To use the text console on the TV again:

```bash
sudo systemctl stop rtspwall
```

To keep the wall from starting at boot, run `sudo systemctl disable --now rtspwall`. Start it again with `sudo systemctl enable --now rtspwall`.

## Optional extras

- **Keep logs across reboots.** Raspberry Pi OS may keep the journal in RAM. `sudo mkdir -p /var/log/journal && sudo systemctl restart systemd-journald` makes it persistent.
- **Upgrade.** Run the `get.sh` command from Step 2 again. Your config is kept.
- **Remove.** `sudo apt remove rtspwall` keeps your config. `sudo apt purge rtspwall` deletes it.

## Next

- [Cameras](cameras.md): per-brand steps and password characters.
- [Configuration](configuration.md): layouts, rotation, display mode.
- [Troubleshooting](troubleshooting.md): when something does not look right.
