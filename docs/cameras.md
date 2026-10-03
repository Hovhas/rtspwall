# Cameras

How to get a working RTSP URL for your cameras, and how to make sure the stream is one rtspwall can show.

rtspwall needs an **H.264** stream of at most 1920x1920 pixels. Use the camera's [sub-stream](glossary.md#sub-stream) when it has one. On a 2x2 wall each tile is at most 960x540 pixels on a 1080p screen, so a main stream gives you nothing extra and uses more of the decoder.

Always check a URL before you add it. `sudo rtspwall probe` asks for the URL at a hidden prompt, reads the codec, size and frame rate, and tells you if it fits.

- **Keep the URL off the command line.** `probe` refuses a URL that contains a password, a query string or a token when you give it as an argument. A command line is saved in your shell history, shows up in `ps` and is logged by `sudo`. Use the hidden prompt, or read the URL from a file with `sudo rtspwall probe - < url.txt`. The flag `--insecure-argv` allows it anyway. Do not use it.
- **Run it with `sudo`.** As root, `probe` reads the stream in a sandbox, as the unprivileged user `rtspwall`, so a broken or hostile stream cannot run as root.
- **Local video files work too.** `sudo rtspwall probe clip.mp4` checks a regular video file. Only RTSP URLs (`rtsp://`, `rtsps://`) and local files can be probed.

> Paths and menus differ between models and firmware versions. Treat the URLs here as starting points and check your vendor's documentation. Only UniFi Protect has been tested by the project. See [Compatibility](compatibility.md) and tell us what works for you with the [camera report form](https://github.com/Hovhas/rtspwall/issues/new?template=camera-report.yml).

## UniFi Protect

1. In Protect, open the camera, then **Settings**, then **Advanced**, then **RTSP**.
2. Enable a stream. Choose **Medium** or **Low**. High is usually too large for a wall.
3. Turn **Enhanced encoding** off for the camera. It switches the camera to H.265, which rtspwall cannot show yet.
4. Copy the URL that Protect shows. It looks like `rtsps://HOST:7441/TOKEN?enableSrtp`.
5. Paste it as it is into `probe` or `add`.

Each quality level has its own token. The token changes when you turn RTSP off and on again, and after a camera reset. If a camera suddenly shows `stream not found (404)`, copy the URL again.

**What rtspwall does with the URL.** By default it keeps the encrypted `rtsps` form on port 7441 and only removes `?enableSrtp`, a parameter that only Protect understands. It logs `UniFi Protect URL: kept rtsps on port 7441 (TLS), Protect-only ?enableSrtp removed`. You do not need to change anything.

The setting `UNIFI_REWRITE` in the config changes this:

| Value | What happens to `rtsps://HOST:7441/TOKEN?enableSrtp` |
|---|---|
| `tls` (default) | Becomes `rtsps://HOST:7441/TOKEN`. The video and the token stay encrypted. |
| `plain` | Becomes `rtsp://HOST:7447/TOKEN`. The token and the video cross your network **unencrypted**. rtspwall logs a warning, and `doctor` warns for each camera. |
| `off` | Used exactly as written. |

`auto` is an older name for `tls`. It still works.

Use `plain` only if `tls` does not play on your setup, and only when the Pi and the cameras sit on an isolated, wired camera network (VLAN). See [SECURITY.md](../SECURITY.md#the-unifi-tls-trade-off) for why. The project tested the `tls` path on hardware with 4 UniFi Protect cameras; all four played. The earlier 6-camera test used the plain rewrite. Tell us how it goes on your setup with the [camera report form](https://github.com/Hovhas/rtspwall/issues/new?template=camera-report.yml).

## Reolink

- RTSP is often off by default. Enable it in the camera's network settings.
- Sub-stream: `rtsp://USER:PASSWORD@IP:554/h264Preview_01_sub`
- Main stream: `rtsp://USER:PASSWORD@IP:554/h264Preview_01_main`
- Many 4K models send H.265 on the main stream. Use the sub-stream, or set the encoding to H.264 in the camera's settings if it offers that.

## Hikvision (and ABUS)

- Sub-stream: `rtsp://USER:PASSWORD@IP:554/Streaming/Channels/102`
- Main stream: `rtsp://USER:PASSWORD@IP:554/Streaming/Channels/101`
- Set the video encoding of the stream to H.264 in the camera's video settings. Newer firmware may default to H.265 or "H.264+".

## Dahua and Amcrest

- Sub-stream: `rtsp://USER:PASSWORD@IP:554/cam/realmonitor?channel=1&subtype=1`
- Main stream: use `subtype=0`.
- Set the encoding of the sub-stream to H.264 in the camera's settings.

## Axis

- A common form is `rtsp://USER:PASSWORD@IP/axis-media/media.amp?videocodec=h264&resolution=640x360`.
- We have not checked this one against a camera. Use your model's documentation.

## Other cameras and ONVIF

Look for "RTSP URL" in the camera's manual or web page, or on the NVR's RTSP page. Choose the sub-stream, set its encoding to H.264, and try the URL with `probe`.

## Frigate and go2rtc

If you run [Frigate](glossary.md#nvr) or [go2rtc](glossary.md#go2rtc), point rtspwall at the restream instead of the camera. The NVR and the wall then share one connection to each camera.

- URL: `rtsp://HOST:8554/STREAM-NAME`. Use the stream name from your go2rtc config.

### Turning H.265 into H.264 with go2rtc

If a camera can only send H.265, go2rtc can convert it. The conversion runs on the machine that runs go2rtc, not on the Pi. Expect real CPU load there, for every converted camera, all day. Use a hardware encoder if that machine has one.

```yaml
streams:
  front_door_h264:
    - ffmpeg:front_door#video=h264#hardware
```

Here `front_door` is an existing stream in your config. The syntax comes from the go2rtc documentation. We have not tested it with rtspwall. Check the go2rtc documentation for your version, then point rtspwall at `rtsp://HOST:8554/front_door_h264`.

## Switching a camera to H.264 or a sub-stream

1. Open the camera's or NVR's web page or app.
2. Find the video or stream settings. The names vary: "Stream", "Encoding", "Video coding", "Enhanced encoding", "Smart codec".
3. Set the stream you will use to **H.264**. Turn off "H.264+", "Smart codec" and "Enhanced encoding" if you see them.
4. Choose a lower resolution if you can (640x360 up to 1280x720 is plenty for a wall).
5. Run `sudo rtspwall probe` and paste the URL. You want `codec: H.264` and a `verdict:` of `PASS`.

## Passwords with special characters

Some characters have a meaning inside a URL. Replace them with their percent-encoded form in the URL.

| Character | Write it as |
|---|---|
| `\|` | `%7C` |
| `@` | `%40` |
| `#` | `%23` |
| `:` | `%3A` |
| `/` | `%2F` |
| `?` | `%3F` |
| `%` | `%25` |
| space | `%20` |

A bare `|` would split the config line into fields, which gives an error. rtspwall also logs the encoding hint when a login fails.

We have not yet verified on both supported FFmpeg versions (5.1 on Bookworm, 7.1 on Trixie) that every one of these characters decodes correctly. If a login keeps failing, set a camera password made of letters and digits only. Use a read-only camera account for the wall where the camera supports it.

## When a camera does not connect

`probe` and the wall log the same short labels.

| You see | Likely cause | What to do |
|---|---|---|
| `login failed (401)` | Wrong user or password, or a special character | Check the account. Encode special characters (table above). |
| `access denied (403)` | The account may not use live view | Give the account live-view or RTSP permission. |
| `stream not found (404)` | Wrong path, or an old UniFi token | Copy the URL again from the camera or NVR. |
| `connection refused` | RTSP is off, or the port is wrong | Enable RTSP. Port 554 is usual. UniFi uses 7441 and 7447. |
| `timeout` or `unreachable` | Wrong address, or a VLAN or firewall in the way | Check the address and that the Pi can reach the camera's network. |
| `not H.264` | The stream is H.265 | [Switch to H.264](#switching-a-camera-to-h264-or-a-sub-stream). |
| `not probing: this URL contains ...` | You gave a URL with a secret as a command-line argument | Run `sudo rtspwall probe` and paste the URL at the prompt. |
| Every path "works" but the picture is wrong | The device answers any path | Use the exact URL from the vendor. |

More in [Troubleshooting](troubleshooting.md).
