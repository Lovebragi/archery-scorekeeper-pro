<p align="right">
  <a href="archery-scorekeeper-backlog.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Archery Scorekeeper — Improvement Backlog

Status snapshot: 1.4.7 released; 1.4.7-beta3 is the current internal test build.
This document records known logic gaps, UX polish and future ideas. Update it when
an item is implemented or re-scoped.

## P0 — logic correctness

- **Separate runtime overrides from saved settings.** Auto training currently
  mutates `s_flow.settings`; the Settings page then shows temporary values and a
  later unrelated save can persist them. Introduce runtime override fields for
  voice/roll entry and clear them at session end or reboot.
- **Finish the automatic reconnect policy.** Current code retries only the last
  saved SSID. Target: on disconnect try all saved SSIDs once; if all fail retry
  the last SSID with 30 s → 60 s → 2 min → 5 min → 10 min backoff; every third
  retry try all saved SSIDs; never scan; stop on connect; manual scan always
  available.
- **Timeout for the automatic training check.** Add an 8–10 s cap; on timeout
  report voice unavailable and continue to training instead of staying on the
  check page.
- **Normalize the voice/roll-entry exclusivity on load.** Older NVS blobs can
  still have both set; force one off when applying settings.
- **Do not write NVS from background tasks.** Already moved the auto-check
  result to the main task; keep this rule for future Wi-Fi/voice work.

## P1 — UX polish

- **Show the full ASR text on failure.** The entry-screen hint is one line and
  truncates long text; use two lines or a scrollable detail view.
- **Adaptive ASR wait.** Keep the 1 s fast-fail for short phrases, extend to
  1.5–2 s for long phrases.
- **Countdown clip consistency.** System/Xiaodou clips are 600 ms but speech
  length and trailing silence still differ; 7 still sounds cut. Regenerate with
  uniform pace or normalize the speech section.
- **Lightweight clock refresh.** Minute changes currently rebuild the whole
  page; update only the status-bar time label.
- **Saved-network management.** Add “clear saved networks” and show which
  networks are saved; stale bad passwords cannot be removed otherwise.
- **Password page discoverability.** Double-click to connect is correct but not
  obvious; consider an explicit “connect” state or hint animation.

## P2 — maintenance

- **Diagnostics voice status.** Currently the last handshake result only. Add a
  timestamp and an on-demand “test now” action.
- **Host/CI coverage.** Cover 12 Settings rows, settings v11, Beta submenu,
  auto-check result application, Wi-Fi password flow and saved-network fallback.
- **Release discipline.** Beta builds go to GitHub prereleases only; submit to
  the community after a stable version is validated.

## Future ideas

- Undo last arrow/end; resume an unfinished session.
- Edit/delete a group from history detail; richer CSV/JSON export and restore.
- Wi-Fi OTA updates; BLE provisioning.
- Chinese/English UI switch.
- ESP32-S3 port for dual-core headroom.
- Power profiling and idle Wi-Fi policy.

## Completed (do not re-open)

- API-key direct ASR, gzip response parsing, 1.2 s failure exit.
- Four countdown voices: Off / System / Xiaodou / Doubao.
- Roll entry; press-triggered navigation; settings row/value redesign.
- Network “Verify voice”; password visibility toggles; reconnect backoff switch.
- Beta submenu with automatic training and automatic reconnect switches.
