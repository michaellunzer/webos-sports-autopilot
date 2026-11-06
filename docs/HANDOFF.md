# Handoff: Sports Autopilot (YouTube TV Multiview, rooted LG webOS)

Snapshot of the project as of 2026-11-06, written so another conversation can pick it up cold.

## Goal
A Homebrew Channel app for a **rooted LG 55UM7300PUA** that controls **YouTube TV Multiview**. When the game with audio focus goes to commercial, it moves focus to a live game. A second mode keeps one game full screen and hops to a live game at each break. Remote buttons control it: Green = on, Red = off, Yellow = Multiview/full-screen mode, Blue = status toast.

## Where things are
- **Repo:** https://github.com/michaellunzer/webos-sports-autopilot (public, `main`). History is linear with no open branches.
- **Releases:** v0.1.0, v0.1.1, v0.1.2, **v0.2.0 (latest)**. Each has an `.ipk` attached.
- **Homebrew Channel repo URL:** `https://michaellunzer.github.io/webos-sports-autopilot/apps.json`
  - Built by `.github/workflows/release.yml` and served by GitHub Pages.
  - On the TV: HBC → Settings → Add repository. From SSH:
    `luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"org.webosbrew.hbchannel","params":{"launchMode":"addRepository","url":"<url>"}}'`
- **IDs:**
  - app `org.sportsautopilot.app`, service `org.sportsautopilot.app.service`
  - on-TV binary `sportsap`
  - data dir `/media/developer/sports-autopilot/` (`config.json`, `ssap-key`)
- **CI:** `.github/workflows/build.yml` runs on every push. It does host unit tests, then cross-compiles with the openlgtv buildroot-nc4 toolchain, then `ares-package`.

### Session constraints (Claude Code cloud)
- **Can't do from the session:**
  - create repos
  - push tags (git push and the API both return 403)
  - change repo settings
  - download the cross toolchain
- **Releasing:**
  ```bash
  gh workflow run release.yml -R michaellunzer/webos-sports-autopilot --ref main -f version=X.Y.Z
  ```
  The workflow creates the tag itself, then builds, publishes the GitHub Release and deploys Pages.
- **No local cross-compile:** TV-side C is syntax-checked against hand-written stub headers for luna-service2 and pbnjson, and the real compile happens in CI.

## Install / update on the TV (over SSH)
```sh
cd /tmp && curl -fL -o sa.ipk https://github.com/michaellunzer/webos-sports-autopilot/releases/download/v0.2.0/org.sportsautopilot.app_0.2.0_all.ipk && luna-send-pub -w 30000 -i 'luna://com.webos.appInstallService/dev/install' '{"id":"com.ares.defaultName","ipkUrl":"/tmp/sa.ipk","subscribe":true}'
/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service org.sportsautopilot.app.service; pkill -x sportsap; true
SA=/media/developer/apps/usr/palm/services/org.sportsautopilot.app.service/sportsap
```
- SSH is `root@<tv-ip>` (the HBC SSH server, port 22). The user also has webOS Dev Manager.
- Gotchas:
  - **Mac vs TV:** `scp` runs on the Mac, not the TV.
  - **BusyBox `timeout`:** different syntax. Use `cmd & sleep N; kill $!`.
  - **Old `node`:** the TV has node v0.12, so use ES5 only.

## What we learned about this TV (verified on hardware)
Model 55UM7300PUA, firmware 05.40.61, webOS 4.5 (sdkVersion 4.10.0), platform W19P.

| Topic | Finding |
|---|---|
| **Video capture of YouTube TV** | ❌ **Blocked (DRM).** `DILE_VT_Create` is refused while YouTube TV plays (single channel or Multiview); PicCap/hyperion-webos is refused too. Works for the regular YouTube app (real frames). HDMI1 gives black frames (HDCP or no signal). `libvtcapture` doesn't exist on webOS 4.5; `libdile_vt` is the backend. |
| **Process name** | LG's `libdile_vt` refuses `DILE_VT_Create` when the process name is longer than 15 chars. That's why the binary is `sportsap`. |
| **Audio** | ❌ `parec -d pcm_output.monitor` records pure silence while YouTube TV plays (hardware audio path). |
| **Screenshot service** | `luna://com.webos.service.tv.capture/executeOneShot` (`{"path","method":"DISPLAY","format":"PNG","width","height"}`) works. YouTube TV's **video is black**, but the user reports **closed captions are visible** in it. (`com.webos.service.capture` doesn't exist on this TV.) |
| **YouTube TV app** | App id `youtube.leanback.ytv.v1`. Runs in **Cobalt** (`/var/palm/jail/youtube.leanback.ytv.v1/.../cobalt`), not WebAppMgr, so there's no web inspector and no DOM access to caption text or tile titles. Regular YouTube is `youtube.leanback.v4`. |
| **Remote input (listening)** | Works. Magic Remote "LGE M-RCU - Builtin" codes: Right 106, Left 105, OK 28, **Back 412**, **Green 399**, **Red 398**. Yellow/Blue are assumed to be 400/401 (not checked). |
| **Key injection via uinput** | ❌ The device registers (`event7`, kbd) but **YouTube TV ignores it**. |
| **Key injection via luna `networkinput/getPointerInputSocket` directly** | ❌ It returns `/tmp/netinput.pointer.sock`, but writing to it does nothing, not even HOME. |
| **Key injection via SSAP** | ✅ **Works.** This is the websocket server on `ws://127.0.0.1:3000`, the same one LG's app and Home Assistant use. Steps: register (one-time Allow prompt), request `ssap://com.webos.service.networkinput/getPointerInputSocket`, connect to the returned `ws://.../netinput.pointer.sock`, send `type:button\nname:RIGHT\n\n`. The pairing key is saved in `/media/developer/sports-autopilot/ssap-key` (paired already). |
| **Home Assistant** | The user's HA has `media_player.living_room` (LG webostv), but it showed "off" / not connected during tests. |

## Architecture (v0.2.0)
```
ESPN play-by-play (curl, every 5 s, per assigned game)  --> src/tv/espn.c
   -> break predictor (src/core/predictor.c), at viewer time = now - stream_delay_s (45 s default)
        marker plays (Official Timeout, Two-minute warning, End Period, End of Half) -> AD
        no new play for 75 s (40 s after score/timeout/punt/turnover/kickoff)        -> AD
        next snap < 20 s away                                                         -> LIVE
   -> autopilot state machine (src/core/autopilot.c), 1 s timer in src/tv/service.c
   -> key queue -> SSAP button presses (src/tv/ssap.c); uinput is an optional fallback
Color buttons: passive /dev/input listener (src/tv/remote_listen.c), no grab
App (frontend/): assign today's ESPN games to the 4 tiles, stream delay ±5 s, status, on/off/mode
Debug page: http://<tv-ip>:8088/ (status.json includes per-game predictor state)
```
- **Predictor accuracy:** on 13 real games from 2026-11-01 it catches about 92% of break time at about 87% precision. The fixture is `tests/data/bal-buf-2026-11-01.csv`.
- **ESPN feed:** `https://sports.core.api.espn.com/v2/sports/football/leagues/nfl/events/{id}/competitions/{id}/plays?limit=25&page=N`. It has `wallclock` timestamps and is about 57 KB per page. The scoreboard (for the game picker) is `site.api.espn.com/.../nfl/scoreboard`, which allows CORS from any origin.
- **Picture path (v0.1):** the score-bug detector (`src/core/detector.c`) and capture code (`third_party/unicapture`, from hyperion-webos, MIT) are still present behind `"detection":"video"`. Kept for a future external HDMI-capture box ("stage 2").
- **Tests (`ctest` in `build-host`):**
  - `test_detector` (synthetic frames)
  - `test_autopilot`
  - `test_predictor` (real-game replay)
  - `test_ssap` (against `tests/fake_ssap.py`)
- **Build checks:** `scripts/build-ipk.sh` checks that the executable exports `log_printf` and `getticks_us`, because the dlopen()ed backends need them.
- **Safety:**
  - `autostart` defaults to false
  - kill switch: `touch /media/developer/sports-autopilot/disabled`
  - autopilot only acts while the foreground app matches `ytv`
  - manual nav keys pause it for 2 min, ignoring keys within 1.5 s of our own injected presses

## Status / not yet verified on the TV
v0.2.0 is released, but **the user has not run it on the TV yet**. Things to confirm:
1. The service can fetch the ESPN feed with the TV's `curl` (HTTPS). The app's *Game data* row shows this, or `dataError` in `status.json`.
2. The service's SSAP client reuses the saved key, and the *Test key* button moves the Multiview highlight.
3. Predictor states match real breaks during a live game, and `stream_delay_s` is calibrated.
4. Full-screen mode: does YouTube TV keep focus on the same game after BACK? Is the 2×2 focus model (UP/LEFT reset, arrow moves) right?
5. Yellow/Blue key codes are 400/401.

## Open thread when this handoff was written
**Closed-caption experiment.** The user's idea is to use YouTube TV captions (visible in screenshots) to tell live play from ads:
- live play has *roll-up* captions: ALL CAPS, incremental, `>>`
- ads have *pop-on* captions: whole sentences, mixed case, gaps

The user captured a time-lapse (a screenshot every 2 s, about 6 min) and **combined the images into a PDF, but it hasn't been shared with the assistant yet**. It wasn't found in Google Drive, and the user interrupted the request to share it. Next steps:
1. Get the PDF, or the user's description: are there captions during ads, and how do they differ?
2. If confirmed, build:
   - `src/tv/screenshot.c`: about 1 fps screenshots, PNG decode (e.g. vendored `stb_image.h`), caption-area crop
   - `src/core/captions.c`: text-presence, roll-up vs pop-on change detection and an upper-case estimate, with host tests on the user's frames
3. Fuse that with the predictor for the focused game, and use caption-detected break starts to auto-calibrate the stream delay.
4. In Multiview, check whether captions show only for the focused tile.

Other ideas discussed:
- **Automatic tile→game assignment:** reading the Multiview line-up is not possible via DOM (Cobalt). It might work by OCR of screenshots.
- **Stage 2:** an external HDMI capture box running the existing picture detector.
- **"Home game" preference.**
