# ESP-Mosaico Flight Radar (Vibe Mode app)

![preview](preview.png)

A green-phosphor radar showing live aircraft around North Brunswick, NJ (or anywhere
you set), installed as a **normal Mosaico app next to Vibe Mode**. Vibe Mode, your
Ideas and your settings stay in place.

* Rotating sweep. Aircraft light up as it passes, then fade. Heading lines show direction; longer means faster.
* Callsign tags on the 6 nearest. Nearest in amber. Table of the 4 nearest.
* Blinking **OVERHEAD!** when a plane is within ~1 mile.
* **Tap the screen** to cycle the range: 2 / 5 / 10 / 25 mi.
* Uses the **Wi-Fi saved in Vibe Mode** automatically.
* Data: [adsb.fi](https://adsb.fi) open data, refreshed every 10 s (personal, non-commercial use).

## Requirements

* ESP-IDF at the commit the Mosaico workspace pins: `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`
* Python 3.10+ and Git 2.28+
* The [esp-mosaico-vibe](https://github.com/esp-mosaico/esp-mosaico-vibe) workspace

## Install

```bat
:: 1. Workspace (once)
cd C:\esp
git clone https://github.com/esp-mosaico/esp-mosaico-vibe.git
cd esp-mosaico-vibe
git submodule update --init submodule/esp-mosaico-utils submodule/esp-mosaico-bsp

:: 2. Copy this folder to  C:\esp\esp-mosaico-vibe\projects\flight_radar

:: 3. Load ESP-IDF, then build + install through Vibe Mode (Mosaico connected by USB-C)
C:\esp\espv6.2esp-idf\export.bat
cd C:\esp\esp-mosaico-vibe
python mosaico.py iris system-update --project projects/flight_radar
```

If `system-update` reports that the board's base system is missing or incompatible,
run `python mosaico.py recover` once, then run `system-update` again.

## Everyday use

| Task | How |
|---|---|
| See logs | `python mosaico.py iris logs --project projects/flight_radar` |
| Reinstall after code changes | `python mosaico.py iris app-update --project projects/flight_radar` |
| Get back to Vibe Mode | Hold the **AI** button while powering on |
| Change location / units / API | `idf.py -C projects/flight_radar menuconfig` → *Flight Radar*, then `system-update` |

Logs go through ESP-Iris over USB (`iris logs`). Plain `idf.py monitor` won't show them,
because the console is on UART in Mosaico apps.

## What differs from a standalone ESP-IDF project

Based on the official `hello_world` template (`mosaico.py project init`), with its
GSP demo UI replaced by an LVGL radar:

* `partitions.csv` and `sdkconfig.defaults` are the template's, unchanged. They keep the Vibe Mode memory layout.
* `main/main.c` starts ESP-Iris and `iris_ota_support_start()` first, so updates and Vibe Mode stay reachable.
* `sdkconfig.application.defaults` = the template's settings plus LVGL fonts, TLS and display settings.
* Wi-Fi is read from Vibe Mode's shared settings (`sysmeta` → `shared` → `wifi_ssid`/`wifi_password`).

## Status

The radar UI and the data parser were compiled and run on a PC with real LVGL 9.5.
The Mosaico layout passes Espressif's `validate_native_layout.py`. This exact project
has **not yet been built with ESP-IDF or installed on hardware**. If a step fails,
paste the error.
