# Changelog

## 1.5.0
- Chime and short vibration when an aircraft first comes within 1 mile (once per aircraft, at most every 30 s). Configurable in menuconfig.
- OVERHEAD! distance configurable (default 1 mile).
- Store packaging: `mosaico-ideas.json`, Idea-grid icon and labels.

## 1.4
- Location precedence: precise Wi-Fi fix, then manual coordinates, then approximate IP fix.
- Location (town, state, ZIP, coordinates, source) shown in the top-left corner.
- No built-in default location; location found once per start-up.

## 1.2
- Fixed a crash when tapping the screen (Wi-Fi ran out of internal RAM while parsing a large reply).
- Automatic location from nearby Wi-Fi networks or the internet address.

## 1.1
- Text kept clear of the screen's rounded corners; clear message when no Wi-Fi is configured.

## 1.0
- First release: live ADS-B radar with sweep, nearest-aircraft table and tap-to-zoom.
