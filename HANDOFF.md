# HANDOFF: Add a Bitcoin Miner Mode to smalltv-mod

## What this project is

This repo is my fork of giovi321/smalltv-mod, open firmware for a GeekMagic-style ESP32 desk cube. It currently has three display modes (stock ticker, plane radar, Claude usage meter), a web UI, and OTA updates. The device this runs on is sitting on my desk, flashed with v2.8.2, working.

**Mission: add a fourth mode called "Miner" that solo-mines bitcoin against a pool, by porting the mining core from NerdMiner_v2 (cloned at ../NerdMiner_v2).** The cube originally shipped as an NMMiner lottery miner doing 1043 KH/s on closed firmware. We are rebuilding that capability inside this open codebase, then trying to beat that number.

## Status

Miner: **done through M5 and proven end to end** — a share accepted by
solo.ckpool.org at difficulty 1.00. Hybrid engine (SHA peripheral on core 0,
software on core 1) at ~375 KH/s, up from ~50 KH/s dual-core software. Every
optimization was measured on the device, and the ones that lost are recorded in
`MinerShaHw.cpp` so nobody retries them.

The 1043 KH/s target turned out not to be a like-for-like number. Three
compressions per nonce are forced on this chip (its SHA engine cannot be seeded
with a midstate), and at the measured 92 cycles each that caps the hardware at
869 KH/s **even with the CPU doing nothing**. NMMiner's 1035 sits 19% above that
ceiling, and matches what you get counting SHA-256 compressions rather than
nonce attempts — 375 KH/s of nonces is ~1.13 M compressions/s. The realistic
floor for this silicon is ~447 KH/s and we are at ~84% of it.

Phase 2: **B1-B3 done.** The lid pad is discovered from the web UI (Touch tab)
rather than needing a diagnostic build, and tap / double-tap / long-press are
wired globally. B4 is partly there: radar cycles its range on long-press,
ambient steps patterns, flappy flaps.

Phase 3: **notification flash done** (`POST /notify`). Four modes have landed
beyond the original three: clock (SNTP wall time), Spotify now-playing with
album art and auto-focus, ambient (five no-network patterns), and flappy bird
on the lid pad. The rest of the backlog below is untouched.

Flash budget: 93.7% of the OTA slot, ~99 KB free. It was 95.9% before a cleanup
pass that gzipped the web UI (-33 KB), dropped the mining benchmark scaffolding
(-13 KB) and replaced the fully unrolled software SHA with a compact midstate
version (-25 KB). That last one cost ~27% of the software path, not the ~3%
first claimed here — the arithmetic divided by the post-change total instead of
the pre-change one. It was recovered by moving the hot functions to IRAM
(`MINER_HOT` in MinerJob.h); the compact version stays. Keep an eye on this
budget: it is the binding constraint on every remaining feature, and
`tools/gen_webui.py` means UI additions are nearly free.

## Invariants worth not relearning

These are the failure modes this codebase actually produces, and the guards now
in place against them. Adding a feature means touching the guard, not routing
around it.

- **One rule, one place.** A list written twice is the single largest source of
  bugs here: the mode token ladders, the carousel ticks, the album-art descale
  rule and the web UI's feature lists each shipped broken because one copy was
  updated and the other was not. They are now single tables — `kModeTokens` and
  `kCarousel` in `Settings.cpp`, `albumArtFit()` in `AlbumArt.h`, `FEATURES` in
  `webui.html`. Add a row; do not add a branch.
- **The page must parse.** A JS syntax error blanks every tab while the device
  runs perfectly, including the tab you would use to flash a fix. That shipped
  once. `tools/gen_webui.py` now fails the build on unbalanced tags, duplicate
  top-level functions, unparseable JS (`node --check`), an element id the script
  reaches for that the markup never defines, and a `FEATURES` row pointing at a
  control or mode option that does not exist.
- **Zero warnings.** Project sources build clean at `-Wall -Wextra` on all five
  targets, enforced from `platformio.ini`. Two real bugs were sitting in that
  noise. Keep it at zero or the next one hides too.
- **Test the pure part off-device.** Two animations and one parser shipped
  broken this session because the arithmetic was never run anywhere. Anything
  that is string-in/string-out or number-in/number-out belongs in a header a
  host harness can include: `tools/miner_selftest` (job maths),
  `tools/captive_selftest` (portal form parsing). Both run in under a second
  and neither needs the cube.
- **Say why, not just what.** `/api/status` reports the resolved backlight level
  *and the rule that chose it*, the worst loop time since last read, and the
  album-art failure stage. Every one of those exists because a symptom was
  otherwise indistinguishable from three other causes over a phone screen.

## Hardware facts (verified, do not re-derive)

- Board: NM-TV-154, PCB marked "NM-TV-Miner"
- Chip: ESP32-D0WD-V3 rev 3.1, dual core, 240 MHz, 4 MB flash
- Display: 1.54" 240x240 IPS, ST7789, driven via Arduino_GFX
- Build environment: `smalltv_esp32` (already defined in platformio.ini)
- Pin map: already correct in src/board_esp32.h; the repo README also links NMMiner's NM-TV custom firmware guide documenting this board
- This board's variant support was confirmed working by a community tester (repo issue #1)

## Build and flash workflow (remote session)

Development happens in a remote Claude Code session; we don't share a filesystem, so delivery is via git, not a handed-over file.

- Reference codebase: clone https://github.com/BitMaker-hub/NerdMiner_v2 as a sibling of this checkout (`../NerdMiner_v2`). Reference material only — never commit it.
- Build: `pio run -e smalltv_esp32` (install PlatformIO in the session first). Output: `.pio/build/smalltv_esp32/firmware.bin`.
- Verify that build compiles locally before every push — every push costs a CI cycle.
- OTA size check: the app image must fit the 0x180000 (1.5 MB) OTA slot in partitions/smalltv_4mb_ota.csv. v2.8.2 already uses ~90% (~1.42 MB), leaving roughly 130-150 KB for the miner; check the reported Flash usage after every build.
- Delivery: push to the working branch. The repo's GitHub Actions `build` workflow (which also runs on pushes to `claude/**` branches) builds all targets and uploads the `smalltv-mod-firmware` artifact; I download `smalltv-mod-firmware-esp32.bin` from it and flash it myself through the cube's web UI (Update tab, manual OTA upload). I report what happens.
- Serial debugging is available if I plug in USB: `pio device monitor` at 115200. Ask me for serial output when display behavior isn't enough to debug.
- Recovery exists (USB esptool + backups), so a bad build is recoverable. Don't be reckless anyway; a build that kills wifi or the web UI costs me a cable ritual I'd rather not repeat.

## Hard constraints

1. The three existing modes and the web UI must keep working. Mining is an addition, not a replacement.
2. Mining work runs pinned to core 1. Display, wifi, and the web server stay responsive on core 0. The cube must never feel bricked while mining.
3. OTA size: the app image must still fit the OTA partition scheme in partitions/. Check the built firmware.bin size against the partition table before handing me a build. If it doesn't fit, tell me; do not silently change the partition table (that breaks OTA updating and needs a USB flash).
4. Follow the repo's existing patterns: how a mode registers, draws, stores config, and exposes a web UI tab. The new mode should look like it was always there, both in code and on screen.
5. Config lives in the existing config system: pool host, pool port, BTC address, optional worker name. Defaults: solo.ckpool.org, 3333.
6. Commit at every working milestone with a clear message. Never leave the repo in a state where HEAD doesn't build.

## Milestones (in order, each one ends with a build I can flash)

**M1: Skeleton.** A "Miner" mode that registers in the mode list and carousel, draws a placeholder screen, and has a web UI tab with pool/address fields that persist. No networking yet. Proves the mode architecture is understood.

**M2: Stratum client.** Mode connects to the pool over wifi, subscribes, authorizes with my BTC address, and receives jobs. Screen shows connection state and a job counter. No hashing yet. (NerdMiner_v2's stratum implementation is the reference; port, don't reinvent.)

**M3: Mining.** SHA-256 double-hash worker on core 1 iterating nonces on real jobs, submitting shares that meet pool difficulty. Screen shows hashrate, shares found/accepted, uptime. This is the "it's a miner again" moment. Expected starting hashrate: roughly NerdMiner-class, ~150-300 KH/s.

**M4: A screen worth looking at.** Design the miner screen properly in the repo's visual style: hashrate big, best difficulty, shares, pool status, maybe BTC price if another mode already fetches it. Kill any flicker or jank.

**M5: The performance game.** Baseline is whatever M3 achieves; the target is 1043 KH/s (what the closed NMMiner firmware got from this exact chip). Known levers, roughly in order of expected payoff: midstate caching (precompute the first SHA-256 block of the 80-byte header once per job), making full use of both cores without starving the UI, early-exit checks on the second hash, minimizing copies/byte-swaps in the hot loop, loop unrolling. Benchmark honestly on-screen; report each change's measured effect. Also test the ESP32 hardware SHA peripheral vs optimized software and keep whichever measures faster; don't assume.

**Stretch goals, only after M5:**
- Pool failover (second pool config, auto-switch on disconnect)
- Mining stats surfaced in the web UI status page
- Clean up the diff and prepare it as an upstream PR to giovi321/smalltv-mod

## Phase 2 (after M5): The capacitive button

The cube has a capacitive touch pad on top. The stock NMMiner firmware used it to switch pages, so it is wired and readable; smalltv-mod currently ignores it entirely. The ESP32 has native capacitive touch sensing on specific GPIOs (touch0-touch9), so the pad is almost certainly on one of those pins.

**B1: Discovery. DONE** — better than a diagnostic build. Six of the ten ESP32
touch channels are free (the rest drive the display), and the firmware reads all
six live. The Touch tab has "Find the pad": tap the lid for four seconds and it
reports which GPIO moved, with live per-channel readings as a manual fallback.

**B2: Gesture layer. DONE** — `src/Touch.{h,cpp}`. Tap, double-tap and
long-press with debouncing; the baseline is a slow moving average taken only
while released, so drift is handled and a resting finger can never train the
baseline onto itself. Threshold is in the web UI.

**B3: Global grammar. DONE** — in main.cpp, runtime-only (no flash writes, so a
reboot returns to the saved mode):
- Tap: advance to the next mode (manual carousel)
- Long-press: mode-specific context action (each mode registers its own; default no-op)
- Double-tap: display off/on (overrides the night schedule until the next tap)

**B4: Per-mode context actions.** Ticker: toggle portfolio summary. Radar: cycle zoom range. Miner: toggle live stats vs lifetime stats. Add these via whatever hook B3 established, one commit each.

## Phase 3: Mode backlog (prioritized, build in this order)

Each mode is its own milestone: registers in the carousel, has a web UI tab if it needs config, and doesn't degrade the others. Ask me before starting each one; priorities may shift.

1. **Server status mode.** Polls a JSON endpoint on my Mac Mini home server showing service health: Jellyfin up/down, Minecraft player count, Cloudflare tunnel status, Mini CPU/temp. Also write the companion endpoint (a small script/server suitable for running on the Mini via Docker or launchd) as part of this milestone, in a `tools/` or separate folder. Design the JSON schema so adding fields later doesn't require reflashing.
2. **Googly eyes idle mode.** Recreate the stock GeekMagic googly-eyes clock face (eyes that blink/look around, city name + time) as an idle/screensaver mode. Original-style pixel art, drawn by us, not extracted assets. This is a callback and it should feel like one.
3. **Notification flash. DONE** — `POST /notify` with
   `{"text":..., "sec":8, "color":"#rrggbb"}`. Word-wrapped, auto-sized, accent
   bars top and bottom; a lid tap clears it early. The Status tab shows a ready
   curl line and a test button.
4. **Countdown mode.** One or more configurable countdowns (label + date) from the web UI, days/hours remaining, rotates if multiple.
5. **Pomodoro mode.** Long-press starts a 25-minute focus ring that visibly drains; tap dismisses the break alert. Durations configurable. This mode is only worth building after Phase 2, since the button is its whole interface.
6. **Pixel-art/GIF frame mode.** Upload small images or GIFs via the web UI, stored in flash (mind the filesystem budget; warn me before layouts that would fight the OTA partition), displayed as a frame mode. Flash wear note: image writes should be occasional by design, not automatic.

Parking lot (do not build unless I promote them): weather + calendar glance (needs a Mini-side daemon), world clock, dice/coin-flip mode, sports scores, wristbin deal display, "scratch ticket" best-share animation on the miner screen.

## Working style

- Before writing code, produce a short plan of how the existing mode architecture works and where the miner mode hooks in. I want to sanity-check it.
- Small steps, frequent builds. I'd rather flash five boring builds than one exciting broken one.
- When something fails on-device, ask me targeted questions (what the screen shows, whether the web UI loads) or ask for serial logs, rather than guessing across large changes.
- The economics are a joke and we both know it (solo odds at these hashrates are effectively zero). Correctness and hashrate are the score; income is not.
