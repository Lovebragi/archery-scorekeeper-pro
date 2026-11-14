<p align="right">
  <a href="archery-scorekeeper-changelog.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Archery scorekeeper changelog

Release history for the archery scorekeeper application. The repository-wide
[changelog](../CHANGELOG.md) tracks the AI Passport project itself; this file
follows the application's firmware versions.

## 1.5.9 — 2026-11-14

### Added

- Settings now includes a 13-page User Manual covering controls, training, voice entry, timers, history, sight tools, rangefinder calibration, tilt, the reference ruler, the protractor, settings, networking, and common questions.
- Protractor mode now shows a white right-edge guide labelled "此边贴紧靶面，设备与靶面垂直", rotated for landscape use.

### Changed

- The manual uses chapter pages rather than one long scrolling label, so every section is reachable.
- Protractor instruction text is arranged along the right edge for horizontal use.

### Fixed

- Rangefinder calibration recommendation now states 80 cm full face at 18 m instead of calling it a default.
- Metronome voice list corrected to click, male, Xiaolin, Xiaozhang, Doubao, and cat.

## 1.5.8 — 2026-11-14

### Added

- Rangefinder target presets corrected to 40 half (20 cm), 40 full (40 cm), 80 half (40 cm, rings 6-10), 80 full (80 cm) and 122 full (122 cm).
- Rangefinder target tilt 0-15 degrees, with projected-height correction.
- Custom eye-to-screen distance calibration using a known distance and cursor alignment.
- Reference ruler page now combines an inch scale on the left and a millimetre scale on the right, plus a dedicated rotating-line protractor mode.

### Changed

- Rangefinder cursor moves in 1 px steps, measures the clear gap between the yellow lines and reports distance to 0.1 m.
- Calibration page layout: instructions on the left, cursor and live eye distance on the right.
- Reference ruler and protractor are drawn by a single custom-drawn object to keep long-press movement smooth and reduce LVGL memory use.

### Fixed

- Target cycling direction now follows size order.
- Rangefinder calibration no longer loses the stored eye distance when the target preset changes.
- LVGL page reuse and reduced object counts avoid reboots when opening the ruler page or toggling its modes.

## 1.4.6 — 2026-10-30

### Added

- The ten-second countdown voice setting now has four choices: Off / System / Xiaodou / Doubao; Doubao is the default.
- System and Xiaodou clips are normalized to 600 ms; Doubao keeps the original voice clips.

## 1.4.5 — 2026-10-30

### Added

- New Roll entry setting: on the entry and new-training screens, holding up/down changes the ring count, group count or arrows per group continuously at 500 ms per step.
- Settings now uses up/down to select a row and OK to cycle that row's value; holding up/down waits 0.5 s and then moves one row every 0.5 s.
- Home, network, history and date/time navigation now reacts on press; date/time repeats every 300 ms while held.
- With Roll entry on, holding up/down on the entry screen starts immediately with no first-step delay.

### Fixed

- White screen/reboot when scrolling to the bottom of Settings: the 11-row label array is complete.
- The repeat period was reset to 60 ms after the first step, making clock fields scroll far too fast.

## 1.4.3 — 2026-10-30

### Fixed

- Diagnostics/logs showed question marks for runtime text such as the ring character; the font now includes the ASR log vocabulary.

## 1.4.2 — 2026-10-30

### Fixed

- Failed recognition no longer waits 2-3 seconds: the blocking read is aborted 1.2 s after release, so "not recognized" is shown promptly.

## 1.4.1 — 2026-10-30

### Added

- Voice now connects directly with the new-console **API Key**; the provisioning page only asks for the API Key. Legacy AppID/Access Token fields and the LAN proxy entry were removed.

### Fixed

- Empty ASR text: the final packet now uses a negative sequence, and gzip responses are decompressed.
- Long hang after release: TLS read timeout reduced from 15 s to 500 ms, with a 1 s overall wait cap.
- Chinese text truncated to question marks in diagnostics/logs; truncation now respects UTF-8 boundaries.

## 1.4.0 —2026-10-29

### Added

- Match timer: Settings can set `off / 20 / 30 / 40 seconds per arrow` per group. After
  Start there is a one-second pause, two beeps, a ten-second preparation (one beep at its
  end), then a countdown of arrows × seconds per arrow. The last 30 seconds play a C-major
  do-mi-sol arpeggio; three beeps at zero open the recording page.
- A female voice counts the final ten seconds (ten to one), with its own Settings switch.
- The group summary now shows a "Start next group" button, or "View summary" on the last
  group.
- The provisioning page can set the home title (1-8 characters); the default is the built-in archery title.

### Changed

- Settings order is now target, orientation, brightness, off time, record X, group timer,
  ten-second countdown voice, voice input, beep volume and Wi-Fi password storage.

## 1.3.6 —2026-10-28

### Added

- Hold OK on the home screen for about three seconds to open Diagnostics: Wi-Fi
  state, SSID, signal, Bluetooth state, free/minimum heap, largest block, LVGL
  memory, NVS entries, history count, battery, reset reason and uptime.
- Press OK in Diagnostics to view the last 32 in-memory log lines. Lines wrap to
  the panel width, support hold-to-scroll with wrap-around, and unsupported
  glyphs are shown as `?` instead of boxes.

### Fixed

- The diagnostics SSID stayed `--` when the device auto-connected at boot.

### Changed

- The status bar no longer draws a grey line or red cross when disconnected; it
  only shows the four green signal dashes while connected.

## 1.3.5 —2026-10-28

### Fixed

- Wi-Fi is now stopped before light sleep and restarted with automatic reconnect
  on wake. Keeping Wi-Fi connected while entering manual light sleep violates the
  ESP-IDF requirement and could leave the device unable to wake after a long idle.
- A manual "Disconnect" in the network menu is no longer undone by the automatic
  reconnect; it stays disconnected until you connect again.

### Changed

- Status bar: when connected, four green dashes under the battery show signal
  strength; when disconnected or provisioning, a single light-grey line replaces
  the misleading four grey dashes.
- Wi-Fi SSIDs use a bundled Chinese font, so Chinese network names no longer show
  missing-glyph boxes.
- The key that wakes the device is still swallowed until release, so it cannot
  trigger an action.

## 1.3.4 —2026-10-27

### Changed

- The status bar now shows network state with four dashes under the battery:
  colour is the state (green connected, grey disconnected/provisioning) and the
  number of lit dashes is the signal strength (four is strongest).
- Provisioning no longer uses gold, so it cannot be mistaken for connected.

### Added

- Voice credential hints: the entry screen says "voice not configured" when no
  APPID/token is set, and "voice credentials invalid" when the handshake returns
  401/403.
- Recognition logs now include the target id to help diagnose low rings on full
  targets; host tests cover full-target rings 5 to 1.

## 1.3.3 —2026-10-27

### Changed

- Date/time moved from Settings to the home screen, right below History; the
  home menu now has five entries and saving the clock returns home. Settings is
  down to eight rows.
- Fixed the abort/clear confirmation title sitting too high and overlapping the
  status bar.

## 1.3.2 —2026-10-27

### Added

- Wi-Fi memory: up to 10 successful networks are kept (same SSID updates its
  password, the least recently used one is evicted). Saved networks connect
  directly from the device list or the phone setup page without retyping the
  password.
- Settings gained a "Save Wi-Fi passwords" switch, on by default. Turning it off
  clears the saved list and stops auto-reconnect at boot. Passwords are stored in
  readable form in device NVS and can be read by anyone with the device; turn it
  off if that matters to you.

### Changed

- Settings now use a six-row scrolling window with a "1-6 / 9" range label, so
  rows no longer overlap the bottom hints.
- The home menu wraps: down on the last entry returns to the first, up on the
  first jumps to the last.
- The provisioning page's Connect button now sits directly below the Wi-Fi
  password field.
- Connecting to a saved network now shows "connecting →connected + IP".

## 1.3.1 —2026-10-27

### Added

- History supports hold-to-scroll: holding up/down for 400 ms starts auto-repeat
  and releasing stops it; scrolling past the bottom wraps to the first record
  and past the top wraps to the last one.

### Changed

- A long OK press on the history screen now returns home, consistent with the
  other screens.
- A double OK press cycles the history filter (all / today / last 7 days); a
  short OK press still opens a record's detail.

## 1.3.0 —2026-10-26

### Added

- Voice scoring: press and hold the up button on the entry screen for about
  0.2 s, then say the rings for a full or partial end. The device talks to the
  Doubao streaming speech model 2.0 directly; parsing, homophone correction and
  scoring all run on the device, with no PC or LAN proxy.
- The provisioning page gained a "Doubao credentials" section: enter the APPID
  and Access Token; both are stored in NVS and the token is never echoed back.
- Settings gained a "Voice input" switch: when off, the entry screen returns to
  keypad-only operation and never starts a recording.
- Settings gained six beep levels (0% / 20% / 40% / 60% / 80% / 100%); 0% is
  mute, and changing the level plays one preview beep at the new volume.
- Partial ends (the screen says how many arrows remain), whole-end phrases such
  as "all nines" / "the rest are eights", colloquial forms (sa, lia, erhua) and
  common homophones are supported.

### Changed

- Voice needs a Volcano Engine account with the "Doubao streaming speech model
  2.0" service enabled; new accounts get 20 free hours. Without credentials the
  keypad workflow is unchanged.

### Fixed

- On the network, Wi-Fi password and provisioning screens, two minutes without
  input now only turns the backlight off while the hotspot and page stay alive;
  any key restores it immediately instead of leaving the screen lit.

## 1.2.3 —2026-10-26

### Added

- Settings can turn off "Record inner ten": the entry screen then starts at the
  10 ring and the X count is no longer shown.

### Changed

- Every target now reports a miss as 0 instead of showing "miss" on half faces;
  statistics still call it a miss.

## 1.2.2 —2026-10-26

### Fixed

- The phone setup page no longer lets a mobile keyboard capitalize the Wi-Fi
  name or password field, and disables autocorrect, spellcheck and password
  autofill there.

## 1.2.1 —2026-10-26

### Fixed

- Short-pressing OK on the "Phone setup / export" QR page now stops the setup
  hotspot and returns to the Network menu. Previously the press did nothing and
  only a long press could leave the page.

## 1.2.0 —2026-10-25

### Added

- Download the full training history as a UTF-8 CSV from the phone setup page at
  `192.168.4.1`. One row per session covers date, target, groups, arrows,
  score, average, inner tens, misses and per-ring totals, ready for Excel.
- Show the firmware version in the top-left corner.

### Changed

- The Wi-Fi menu entry is now "Phone setup / export"; the setup page offers the
  CSV download next to the network configuration.

## 1.1.0 —2026-10-25

### Added

- Network page: scan nearby Wi-Fi, type the password on the device, or provision
  from a phone through the open `Archery-Setup` hotspot and the `192.168.4.1`
  web page.
- NTP time sync after connecting (UTC+8), with the clock persisted to NVS.
- Top status bar with the current time (HH:MM), a network indicator dot and the
  battery gauge.

### Changed

- Default screen brightness is 20% and screen-off time is 15 s.
- The setup hotspot is open (no password) and exists only while the Network page
  is shown.

### Fixed

- The Network menu could not move the cursor before the first scan.
- Password repeat used the physical key direction without the 180-degree role
  rotation.
- The provisioning page stayed on "scanning" because scans ran while the access
  point was active; it now uses a pre-scan cache and a Refresh button.

## 1.0.0 —first public release

- 40 cm half target, three-spot target and 40/80/122 cm full targets.
- Per-ring arrow counts, group/total/average/inner-ten/miss statistics.
- 300 sessions with timestamps and all/today/last-7-days filters.
