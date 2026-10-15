---
title: Companion app
description: A program on your computer composes the screen (music, notifications, Steam, system stats) and pushes only what changed; the device keeps the clock ticking on its own.
---

The other features run on the device. This one runs on your computer. The companion app (`companion/` in this repository, Python) composes the whole 240×240 screen with plugins and pushes it to the device. The device only copies pixels to the panel and draws one thing itself: the clock. While nothing changes, the computer sends nothing but a small heartbeat.

Because the computer draws the screen, anything goes. Album art, any font with accents, game artwork, anti-aliased text. None of it costs the ESP8266 memory.

The feature is compiled into the ESP8266 theme image (`smalltv_theme`) and the ESP32 images. The lean ESP8266 image leaves it out.

## Quick start

1. In the web UI, open **Display** and pick **Companion app** as the mode. The panel shows *COMPANION — waiting for the PC app*, with the device's own clock and IP address.
2. On the computer, install the app and write a config:
   ```sh
   pip install ./companion          # or: pipx install ./companion
   smalltv-companion init           # writes ~/.config/smalltv-companion/config.toml
   ```
3. Set `host` under `[device]` in that file, then run:
   ```sh
   smalltv-companion run -v
   ```

`smalltv-companion preview -o out.png` renders the screen to a PNG without a device. Use it to try out layouts and fonts. `--screen <name>` picks one screen, and `--notify "text"` draws a sample notification.

To start the app with your desktop session, use the systemd user unit in `companion/contrib/`. It runs inside your session so it can reach the session bus, which the music and notification plugins need.

## What you see on the device

| State | Screen |
|---|---|
| No frame received yet | **COMPANION**, waiting, with the device clock and IP |
| Frames arriving | the companion's screen, with the clock in its reserved box |
| No request for *offline after* seconds (Companion tab, default 120) | **COMPANION OFFLINE**, with the device clock and IP |

Whenever the device paints over the companion's screen, the app notices within one heartbeat (5 s) and sends a full frame again. That happens on a reboot, on a mode change, on a settings save, and on the offline screen.

Companion mode is not part of the carousel, for the same reason the theme clock is not: the companion owns the whole screen.

## Configuration

`companion/smalltv_companion/config.example.toml` documents every key. The essentials:

```toml
[device]
host = "192.168.1.50"
# password = "..."            # only if the web UI has a password (digest auth)

[clock]                       # drawn by the device, once a minute
box = [0, 0, 240, 64]         # x, y, w, h
font = "Inter:bold"           # a path or a fontconfig name
size = 52
color = "#ffffff"

[[screen]]                    # screens rotate; empty ones are skipped
name = "music"
duration = 20
  [[screen.widget]]
  plugin = "media"
  box = [0, 64, 240, 176]

[plugins.notifications]       # interrupts any screen when one arrives
duration = 8
```

### Built-in plugins

| Plugin | Shows | Notes |
|---|---|---|
| `media` | cover art, title, artist, progress | Any MPRIS player: Spotify, browsers, mpv, VLC. Hidden while nothing plays. |
| `notifications` | each desktop notification, then the recent ones | Watches the session bus as a monitor and leaves the notification daemon alone. Covers `notify-send`, Electron/Chrome/Firefox, GNOME apps and Flatpak. Set `ignore` / `only` by app name. |
| `steam` | the game you are playing with its header art, or which friends are in game | Needs a Steam Web API key and your SteamID64. |
| `system` | CPU, RAM, temperature | `psutil`. |
| `text` | fixed text or a shell command's output | The smallest example to copy. |

### Writing a plugin

A plugin is a class with a name, a `poll()` that returns `True` when something visible changed, and a `render(ctx)` that draws into `ctx.image`. That image is exactly the size of the widget box.

```python
from smalltv_companion.plugins import Plugin

class Weather(Plugin):
    name = "weather"
    interval = 600

    async def poll(self):
        new = await fetch_temperature(self.config["city"])
        changed, self.temp = new != getattr(self, "temp", None), new
        return changed

    def render(self, ctx):
        ctx.text((ctx.w // 2, ctx.h // 2), f"{self.temp:.0f}°", 64, bold=True, anchor="mm")
```

Register it in your own package's `pyproject.toml`:

```toml
[project.entry-points."smalltv_companion.plugins"]
weather = "my_package.weather:Weather"
```

Then use `plugin = "weather"` in a widget. Its `[plugins.weather]` table arrives as `self.config`.

Optional parts:
- `available()`: return False to hide the plugin.
- `interrupt()`: return an `Interrupt(duration, priority)` to take over the content area. `notifications` uses this.
- `start()` and `stop()`: open and close connections.
- `self.changed()`: event-driven plugins call this instead of polling.

## Protocol

Every route sits behind the web UI password, when one is set (digest auth). Every reply is JSON shaped as `{"ok": bool, "epoch": n, "error"?: "..."}`.

| Route | Body | Purpose |
|---|---|---|
| `POST /api/companion/rect?x=&y=&w=&h=` | multipart, one file: `w×h` pixels, RGB565 little-endian, row-major | Draws one rectangle. Answers `409 not_active` when another mode is on screen. |
| `POST /api/companion/glyphs` | multipart, one file: an `SCG1` atlas (≤ 64 KB) | The clock's digits, `0`–`9` and `:`, pre-rendered over the clock background. |
| `POST /api/companion/clock` | JSON `{enabled, x, y, w, h, bg, fg}`, with the colours as RGB565 integers | Where the device draws the clock. `fg` is only used by the built-in font fallback. |
| `GET /api/companion/status[?hb=1]` | none | State, epoch, clock and free heap. `hb=1` counts as a heartbeat. |

The `epoch` goes up every time the device paints over the companion's content. A client that remembers the epoch of its last push and compares it on each heartbeat knows when to resend everything.

The rectangle is streamed. ESP8266WebServer hands a multipart file to the firmware in 2 KB chunks, and the firmware sends each chunk to the panel a few rows at a time. A full frame is 115 KB, and it never sits in RAM; the upload costs a 2 KB buffer while it runs.

`SCG1` layout, little-endian:

| Offset | Type | Content |
|---|---|---|
| 0 | 4 bytes | `"SCG1"` |
| 4 | u8, u8, u16 | glyph count (11), height, reserved |
| 8 | 11 × u8 | glyph widths |
| 19 | … | the glyphs' pixels in order, each `width × height` RGB565 |

Without an atlas, the clock is drawn in the built-in font. The same happens when the atlas does not fit the box, or when the clock is not synced yet (shown as `--:--`).

### Measured on an ESP8266 (SmallTV Ultra)

| Push | Time |
|---|---|
| Full frame, 240×240 | 0.6–0.8 s |
| Progress bar, 216×16 | ~65 ms |
| Typical update (a few changed tiles) | 0.1–0.3 s |

Free heap with the companion streaming: about 22–24 KB, steady over time. The feature adds about 0.8 KB of static RAM to the image.
