# Sports Autopilot for YouTube TV (rooted LG webOS)

A Homebrew Channel app and native service for a **rooted LG TV** (built for the 55UM7300PUA, webOS 4.5). It watches **YouTube TV Multiview** and moves the audio/focus to another game whenever the current one cuts to a commercial. A second mode keeps one game **full screen** and hops to a live game at each break.

Everything runs **on the TV**. The service captures small video frames with the same capture backends as [PicCap / hyperion-webos](https://github.com/webosbrew/hyperion-webos). It finds commercials by looking for the score bug and presses remote keys through a virtual input device.

> Status: **v0.1, untested on real hardware.** The detector and state machine pass unit tests on synthetic frames. The thresholds still need tuning on real games (see [Tuning](#tuning-with-real-games)).

## Remote buttons

| Button | Action |
|---|---|
| 🟢 **Green** | Autopilot **on** |
| 🔴 **Red** | Autopilot **off** |
| 🟡 Yellow | Switch mode: Multiview ↔ Full screen |
| 🔵 Blue | Show a status toast (mode and each game's LIVE/AD state) |

- Every press shows a toast on screen.
- If you use the arrows, OK or Back while autopilot is on, it **pauses for 2 minutes** so it doesn't fight you. Then it re-syncs by moving focus to the top-left tile.
- The service starts when you open the app. To have the color buttons work right after a reboot, set `autostart` to `true`. Even then, autopilot itself is always **off** after a boot.

**How to use:** open a Multiview in YouTube TV and press **Green**. In full-screen mode, also start from the Multiview screen. Autopilot opens a live game, and at each commercial it presses Back, picks another live game and opens it.

## How it works

```
capture (vtcapture / dile_vt, 320x180 @ 3 fps)
   -> detector: split into 4 tiles (or 1 when full screen)
        per pixel: "still + high-contrast" persistence (score bugs are static while the field moves)
        score = static pixels in top/bottom bands - static pixels in the rest of the picture
        black frames reset the history; hysteresis (2 s to LIVE, 4 s to AD)
   -> autopilot state machine: focused game AD -> move to the live game that most
        recently came back from break (20 s minimum between switches)
   -> key queue -> /dev/uinput virtual remote (LEFT/RIGHT/UP/DOWN/ENTER/BACK)
```

- Autopilot only acts while a `youtube` app is in the foreground, and it stops capturing when the screen is off.
- YouTube TV builds Multiview on its servers, so the TV decodes a single stream and one captured frame shows all four games.

| Path | What |
|---|---|
| `src/core/` | Portable detector, autopilot and frame I/O (unit-tested on a PC) |
| `src/tv/` | webOS service: luna API, capture, remote listener, virtual remote, debug page, recorder |
| `third_party/unicapture/` | Capture layer and video backends copied from hyperion-webos (MIT) |
| `frontend/` | The app in the Homebrew Channel launcher (status, on/off, mode, record, grant root) |
| `tools/replay.c` | Run the detector over recorded frames and print a CSV timeline |
| `tests/` | Synthetic-frame tests |

## Safety, uninstall and recovery

- **What it touches:** it's a normal app plus a userland service. It doesn't modify firmware, the bootloader or system partitions. It only writes:
  - `/media/developer/sports-autopilot/` (settings and recordings)
  - `/var/lib/webosbrew/init.d/sportsautopilot`, a boot link that exists only when `autostart` is on
- **Kill switch:**
  ```sh
  touch /media/developer/sports-autopilot/disabled
  pkill -f '[s]ports-autopilot$'
  ```
  The service then refuses to start. Delete the file to re-enable it.
- **Uninstall:** remove the app from the launcher or the Homebrew Channel, then run:
  ```sh
  rm -rf /var/lib/webosbrew/init.d/sportsautopilot /media/developer/sports-autopilot
  ```
- **Recovery:** if a boot hook ever crashed the TV during startup, the Homebrew Channel's failsafe mode skips all hooks on the next boot and opens a telnet shell. Remove the link above, then delete `/var/luna/preferences/webosbrew_failsafe`.
- **Biggest real risk:** the capture driver (the same one PicCap uses) could freeze video or reboot the TV on some firmware/quirk combinations. A reboot clears it. Try other `quirks` one at a time.
- **Emulator:** LG's webOS TV emulator can't help here. It has no root, no Homebrew Channel, no ARM native code, no capture driver and no YouTube TV. Test in stages on the real TV instead:
  1. SSH diagnostics (below)
  2. The debug page with autopilot off
  3. Green during a game

## Phase 0: check that your TV can capture YouTube TV (do this first)

YouTube TV is DRM-protected, and on some firmware the capture returns black frames. Check this before anything else.

1. Install the ipk (see below). Open **Sports Autopilot** and press **Grant root** once.
2. Start YouTube TV Multiview with live games, then SSH into the TV (`ssh root@<tv-ip>`) and run:
   ```sh
   /media/developer/apps/usr/palm/services/org.sportsautopilot.app.service/sports-autopilot --capture-test 10
   ```
   The last line is the verdict: `capture works`, `all black` or `no frames`. Copy `/tmp/sports-autopilot-capture-*.ppm` to a PC and open them to see what was captured.
   - If frames are black, try the other backend or a quirk in `/media/developer/sports-autopilot/config.json`, then rerun:
     - `"video_backend": "libdile_vt"`
     - `"quirks": 1` / `2` / `4` (dile_vt)
     - `"quirks": 256` (vtcapture)
3. Find your color-button codes:
   ```sh
   .../sports-autopilot --listen-keys
   ```
   Press Red, Green, Yellow, Blue, the arrows, OK and Back. The defaults are Linux `KEY_RED=398`, `KEY_GREEN=399`, `KEY_YELLOW=400`, `KEY_BLUE=401`, arrows `105/106/103/108`, `ENTER=28`, `BACK=158`. If your remote differs, set `key_*` in the config.
   - The UM7300's basic IR remote has color buttons. A Magic Remote only offers them in the on-screen "…" menu.
4. Check that injected keys reach YouTube TV. With Multiview on screen, run:
   ```sh
   .../sports-autopilot --send-key RIGHT
   ```
   Focus should move to the next game. If nothing happens, try `--send-key 106` and compare against the codes from step 3.

## Install from the Homebrew Channel

Homebrew Channel → **Settings** → **Add repository**, then enter:

```
https://michaellunzer.github.io/webos-sports-autopilot/apps.json
```

**Sports Autopilot** then shows up in the app list. Every `v*` tag builds a release and updates this repository automatically (`.github/workflows/release.yml`).

## Build and install

GitHub Actions builds the `.ipk` on every push (artifact **sports-autopilot-ipk**). To build locally, you need the [buildroot-nc4 toolchain](https://github.com/openlgtv/buildroot-nc4/releases) and `npm i -g @webosose/ares-cli`. Then run:

```bash
git submodule update --init --recursive
```

```bash
TOOLCHAIN_FILE=/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake scripts/build-ipk.sh
```

```bash
scripts/deploy.sh <tv-ip>
```

To run the host unit tests and build the replay tool (no TV needed):

```bash
cmake -S . -B build-host && cmake --build build-host && (cd build-host && ctest --output-on-failure)
```

## Debug page

While capture runs, open `http://<tv-ip>:8088/` on a phone or PC. It shows:
- the live captured frame with tile boxes (green = LIVE, red = AD, gray = unknown, white = focused)
- the score-bug search bands
- all detector numbers

Opening the page keeps capture running for 60 s even when autopilot is off. Set `"debug_http_port": 0` to turn the page off. It serves your TV picture to your LAN.

## Tuning with real games

1. During a game window, press **Start recording** in the app, or call:
   ```sh
   luna-send -n 1 luna://org.sportsautopilot.app.service/record '{"start":true}'
   ```
   Frames are saved at 1 fps to `/media/developer/sports-autopilot/rec/<timestamp>/`, up to 1800 frames (about 300 MB), along with a `labels.csv` of what the detector decided.
2. Copy the folder to a PC and replay it with different parameters:
   ```bash
   build-host/replay --param live_score_on=0.03 --param ad_hold_ms=5000 rec/20261102-131500 > timeline.csv
   ```
3. Put the values that work into the config, either through the `setSettings` luna method or by editing `config.json` and restarting the service.

Settings live in `/media/developer/sports-autopilot/config.json`. See `src/tv/settings.c` for every key and its default. Notable ones:

| Key | Default | Meaning |
|---|---|---|
| `live_score_on` / `live_score_off` | 0.025 / 0.010 | Score thresholds for LIVE / AD evidence |
| `live_hold_ms` / `ad_hold_ms` | 2000 / 4000 | Hysteresis |
| `band_top` / `band_bottom` | 0.22 / 0.25 | Where score bugs are searched (fraction of tile height) |
| `mv_inset_*`, `mv_gutter` | 0 | Multiview tile geometry, if the grid doesn't fill the screen |
| `min_switch_interval_ms` | 20000 | Never switch more often than this |
| `manual_pause_ms` | 120000 | Hands-off time after you use the remote |
| `foreground_apps` | `youtube` | Comma-separated substrings of app ids where autopilot may act |
| `autostart` | false | Start the service at boot, so the color buttons work without opening the app first |

## Luna API

All methods are on `luna://org.sportsautopilot.app.service/`:

| Method | Payload |
|---|---|
| `status` | — |
| `getSettings` | — |
| `setSettings` | settings JSON |
| `enable` | `{"enabled":true}` |
| `setMode` | `{"mode":"multiview"\|"fullscreen"}` |
| `record` | `{"start":true}` |
| `preview` | — (capture for 60 s) |
| `sendKey` | `{"key":"RIGHT"}` |

## Known unknowns and roadmap

These have to be confirmed on the real TV and app. All of them are configurable:
- whether the capture works with DRM (Phase 0)
- the key codes for the color buttons
- whether YouTube TV moves Multiview focus with the arrow keys and keeps focus on the same game after Back
- the exact grid geometry

Roadmap:
- **Learned score-bug mask:** remember where each game's bug sits so the detector reacts faster and ignores static logos in ads.
- **Multiview vs. full-screen screen detection:** look for grid gutters so the autopilot can re-sync after manual use.
- A small classifier trained on recorded frames.
- **"Home game" preference:** return to your main game when it comes back from break.
- An SSAP (`ws://localhost:3000`) key fallback in case uinput keys aren't accepted.
