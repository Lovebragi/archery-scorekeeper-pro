<p align="right"><a href="ui-input-contract.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# UI and Input Contract

Every screen and menu in this application follows these rules. They exist
because the same defects have recurred: overlapping header text, key roles that
ignore screen rotation, actions firing on release, and menus that do not wrap.

## Header layout

- Reserve a top band for the status bar: time and battery.
- Place the page title below that band, or move the time to the left corner when
  the title is centered.
- Never draw the title and the status bar at the same `y` coordinate.
- The version string belongs on the home screen only.
- A page that needs the full panel (for example the ruler) must explicitly hide
  the status bar and must not leave a stale clock-label pointer.

## Key roles

- Page handlers receive only `ARCHERY_KEY_UP`, `ARCHERY_KEY_DOWN`,
  `ARCHERY_KEY_OK`, and `ARCHERY_KEY_OK_LONG`.
- Physical-to-logical mapping, including 180-degree screen rotation, happens once
  in the application input layer. A page must not inspect `rotate` or physical
  button ids.
- Test every menu with `rotate = 0` and `rotate = 1`; the visual direction must
  match the key direction in both cases.

## Press semantics

- Navigation and value changes fire on `BSP_BTN_PRESS`, not on release.
- After a press-triggered action set `s_repeat_used` so the release click is
  swallowed.
- Long press triggers only the long action. A page that has both short and long
  OK semantics must not execute the short action twice.
- Repeat timers must stop on release and must not queue work after the page
  changes.

## Menu wrapping

- Menus wrap: Up on the first row goes to the last; Down on the last row goes to
  the first.
- Scanned Wi-Fi lists may clamp instead of wrapping, so the user cannot
  accidentally connect while trying to reach a menu row.
- Any page with more than one selectable row needs a test for both boundaries.

## Definition of done for a UI change

```text
[ ] Title, time, battery, version do not overlap
[ ] rotate 0/180 produce the same logical direction
[ ] Up/Down/OK respond on press, not release
[ ] Menu wraps at both ends
[ ] Full-page redraw is not used for continuous value changes
[ ] validate.sh --static passes
```

## Continuous controls

Do not rebuild the whole screen for every repeat tick. For a continuously moving
element, keep object handles and update only positions/text; otherwise an 80 ms
repeat can exhaust LVGL memory and reboot the device.
