# Sports Autopilot for YouTube TV (rooted LG webOS)

A Homebrew Channel app and native service for a **rooted LG TV** (built for the 55UM7300PUA, webOS 4.5). It watches **YouTube TV Multiview** and moves the audio/focus to another game whenever the current one cuts to a commercial. A second mode keeps one game **full screen** and hops to a live game at each break.

Everything runs **on the TV**. The service follows each game's live play-by-play (ESPN's public feed) to work out when a game is in a commercial break. It presses remote buttons through the TV's own remote-control server, the same one LG's phone app and Home Assistant use.

> Status: **v0.2, partly tested on a 55UM7300PUA.** On that TV:
> - the color buttons are received
> - key presses through the remote-control server reach YouTube TV
> - **video capture and audio are blocked while YouTube TV plays (DRM)**, which is why v0.2 switched to game data
>
> The break predictor is tuned on 13 real games but hasn't been used live yet.

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

**How to use:**
1. Open a Multiview in YouTube TV.
2. Open **Sports Autopilot** and, under *Games in Multiview*, set each tile to the game YouTube TV shows there. Press OK on a tile to cycle through today's games.
3. Go back to YouTube TV and press **Green**.

The first time autopilot presses a button, the TV asks you to **Allow** it. Accept, and it won't ask again.

In full-screen mode, also start from the Multiview screen. Autopilot opens a live game, and at each commercial it presses Back, picks another live game and opens it.

**Stream delay:** YouTube TV runs behind the live game, typically 30–60 s. If autopilot switches before the commercial starts on your screen, raise *Stream delay* in the app. If it switches late, lower it. The default is 45 s.

## How it works

```
ESPN play-by-play (each assigned game, every 5 s)
   -> break predictor, evaluated at "viewer time" = now - stream delay:
        Official Timeout / Two-minute warning / End of quarter / half  -> commercial
        no new play for 75 s (40 s after a score, timeout, punt, turnover or kickoff) -> commercial
        next snap less than 20 s away                                  -> live again
   -> autopilot state machine: focused game at commercial -> move to the live game that
        most recently came back from break (20 s minimum between switches)
   -> key queue -> TV remote-control server (SSAP, ws://127.0.0.1:3000) -> LEFT/RIGHT/UP/DOWN/ENTER/BACK
```

- On 13 games from 2026-11-01, this flags about 92% of commercial-break time, and about 87% of what it flags is a real break. The rest is mostly reviews and injury stoppages.
- Autopilot only acts while YouTube TV (`youtube.leanback.ytv.v1`) is in the foreground.
- **Why not look at the picture?** That was v0.1's approach: find the score bug in captured frames. The code is still here, with `"detection": "video"`, and works for unprotected video. But webOS 4.x refuses video capture during YouTube TV playback, and its audio bypasses the recordable path. The picture-based detector is kept for a future external-capture box (an HDMI capture card in front of a streaming stick).

| Path | What |
|---|---|
| `src/core/` | Portable break predictor, picture detector, autopilot and frame I/O (unit-tested on a PC) |
| `src/tv/` | webOS service: luna API, ESPN poller, SSAP key sender, remote listener, capture, debug page |
| `third_party/unicapture/` | Capture layer and video backends copied from hyperion-webos (MIT) |
| `frontend/` | The app in the Homebrew Channel launcher (status, on/off, mode, record, grant root) |
| `tools/replay.c` | Run the detector over recorded frames and print a CSV timeline |
| `tests/` | Predictor replay on a real game, autopilot logic, picture-detector tests, SSAP client against a fake TV server |

## Safety, uninstall and recovery

- **What it touches:** it's a normal app plus a userland service. It doesn't modify firmware, the bootloader or system partitions. It only writes:
  - `/media/developer/sports-autopilot/` (settings and recordings)
  - `/var/lib/webosbrew/init.d/sportsautopilot`, a boot link that exists only when `autostart` is on
- **Kill switch:**
  ```sh
  touch /media/developer/sports-autopilot/disabled
  pkill -x sportsap
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

## Diagnostics (picture capture; not needed for game-data mode)

YouTube TV is DRM-protected, and on some firmware the capture returns black frames. Check this before anything else.

1. Install the ipk (see below). Open **Sports Autopilot** and press **Grant root** once.
2. Start YouTube TV Multiview with live games, then SSH into the TV (`ssh root@<tv-ip>`) and run:
   ```sh
   /media/developer/apps/usr/palm/services/org.sportsautopilot.app.service/sportsap --capture-test 10
   ```
   The last line is the verdict: `capture works`, `all black` or `no frames`. Copy `/tmp/sports-autopilot-capture-*.ppm` to a PC and open them to see what was captured.
   - If frames are black, try the other backend or a quirk in `/media/developer/sports-autopilot/config.json`, then rerun:
     - `"video_backend": "libdile_vt"`
     - `"quirks": 1` / `2` / `4` (dile_vt)
     - `"quirks": 256` (vtcapture)
3. Find your color-button codes:
   ```sh
   .../sportsap --listen-keys
   ```
   Press Red, Green, Yellow, Blue, the arrows, OK and Back. The defaults are Linux `KEY_RED=398`, `KEY_GREEN=399`, `KEY_YELLOW=400`, `KEY_BLUE=401`, arrows `105/106/103/108`, `ENTER=28`, `BACK=158`. If your remote differs, set `key_*` in the config.
   - The UM7300's basic IR remote has color buttons. A Magic Remote only offers them in the on-screen "…" menu.
4. Check that injected keys reach YouTube TV. With Multiview on screen, run:
   ```sh
   .../sportsap --send-key RIGHT
   ```
   Focus should move to the next game. If nothing happens, try `--send-key 106` and compare against the codes from step 3.

## Install from the Homebrew Channel

Homebrew Channel → **Settings** → **Add repository**, then enter:

```
https://michaellunzer.github.io/webos-sports-autopilot/apps.json
```

**Sports Autopilot** then shows up in the app list. To publish a new version, run the **Release** workflow from the Actions tab with a version number, or push a `v*` tag. It builds the ipk, creates the GitHub Release and updates this repository (`.github/workflows/release.yml`).

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

## Settings

Settings live in `/media/developer/sports-autopilot/config.json`. Change them through the app, the `setSettings` luna method, or by editing the file and restarting the service. See `src/tv/settings.c` for every key and its default. Notable ones:

| Key | Default | Meaning |
|---|---|---|
| `tile_events` / `tile_labels` | empty | ESPN event ids and names of the games in tiles 1–4 (set by the app) |
| `stream_delay_s` | 45 | How far YouTube TV runs behind the live game |
| `idle_break_s` / `trigger_idle_s` | 75 / 40 | Seconds without a new play that mean a commercial (the second value applies after a score, timeout, punt, turnover or kickoff) |
| `resume_lead_s` | 20 | Treat a game as live again this long before the next snap |
| `poll_interval_s` | 5 | How often ESPN is polled |
| `detection` | `gamedata` | `gamedata`, or `video` (picture-based; blocked by DRM for YouTube TV on webOS 4.x) |
| `use_uinput` | false | Press keys with a virtual input device instead of the TV's remote-control server. YouTube TV ignores the virtual device on webOS 4.5. |
| `min_switch_interval_ms` | 20000 | Never switch more often than this |
| `manual_pause_ms` | 120000 | Hands-off time after you use the remote |
| `key_back` | 412 | Linux code of the remote's Back key, used to spot manual use |
| `foreground_apps` | `ytv` | Comma-separated substrings of app ids where autopilot may act |
| `autostart` | false | Start the service at boot, so the color buttons work without opening the app first |

To re-tune the predictor offline, export a game's play-by-play like `tests/data/bal-buf-2026-11-01.csv` and run `build-host/test_predictor <file.csv>`. It prints recall and precision against approximate break times.

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

These still need confirming on the real TV:
- whether YouTube TV keeps Multiview focus on the same game after Back (affects full-screen mode)
- typical stream delay, so the default can be tuned
- whether the ESPN feed's lag stays below the stream delay during busy Sunday windows

Roadmap:
- **Stage 2, external capture box:** run the picture-based detector (`src/core/detector.c`) on a small computer with an HDMI capture card, and send results to the TV.
- **Automatic tile assignment:** read YouTube TV's Multiview line-up instead of picking the games in the app.
- **"Home game" preference:** return to your main game when it comes back from break.
- **Stream-delay auto-calibration.**
