# ESP-Mosaico Flight Radar (Vibe Mode app)

![preview](preview.png)

A green-phosphor radar showing live aircraft around wherever the Mosaico is,
installed as a **normal Mosaico app next to Vibe Mode**. Vibe Mode, your
Ideas and your settings stay in place.

* Rotating sweep. Aircraft light up as it passes, then fade. Heading lines show direction; longer means faster.
* Callsign tags on the 6 nearest. Nearest in amber. Table of the 4 nearest.
* Blinking **OVERHEAD!**, a two-note chime and a short vibration when an aircraft first comes within 1 mile (once per aircraft, at most every 30 s).
* **Tap the screen** to cycle the range: 2 / 5 / 10 / 25 mi.
* Uses the **Wi-Fi saved in Vibe Mode** automatically.
* **Finds its own location** at every start-up: nearby Wi-Fi networks first, then the internet (IP) address. Town, state, ZIP, coordinates and source are shown in the top-left corner.
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
| Reinstall after code or setting changes | `python mosaico.py iris system-update --project projects/flight_radar` (use this rather than `app-update`, which refuses boards whose Vibe Mode is newer than the toolkit) |
| Get back to Vibe Mode | Hold the **AI** button while powering on |
| Change location / units / API | See [Settings (menuconfig)](#settings-menuconfig) |
| Set a location by hand | menuconfig → *Flight Radar* → *Manual latitude/longitude*. Used unless a precise Wi-Fi fix is found. |

Logs go through ESP-Iris over USB (`iris logs`). Plain `idf.py monitor` won't show them,
because the console is on UART in Mosaico apps.

## Settings (menuconfig)

All radar settings live in one menu. Open it from a normal Command Prompt or PowerShell
window (not an IDE output pane: menuconfig needs a real terminal).

```bat
:: 1. Load ESP-IDF (every new window)
C:\esp\espv6.2esp-idf\export.bat

:: 2. Go to the workspace and open the menu for this project
cd C:\esp\esp-mosaico-vibe
idf.py -C projects/flight_radar menuconfig
```

The first time, this configures the project first, which takes a minute. Then:

1. Use **↓** to move to **Flight Radar** (near the bottom of the top-level list) and press **Enter**.
2. Move to a setting and press **Enter** to edit it, or **Space** to toggle a choice.
   Type the value and press **Enter**.
3. Press **/** at any time to search, e.g. `FR_MANUAL_LAT`.
4. Press **S**, then **Enter**, to save. Press **Q** (or **Esc** twice) to quit.

Then build and install the new settings:

```bat
python mosaico.py iris system-update --project projects/flight_radar
```

| Setting (symbol) | Default | What it does |
|---|---|---|
| Fallback Wi-Fi network name (`FR_WIFI_SSID`) | *(empty)* | Leave empty to use the Wi-Fi saved in Vibe Mode |
| Fallback Wi-Fi password (`FR_WIFI_PASSWORD`) | *(empty)* | Only with the SSID above |
| Place-name lookup URL (`FR_GEOCODE_URL`) | `https://nominatim.openstreetmap.org/search` | Turns a `radar_location` ZIP/place into coordinates |
| Manual latitude (`FR_MANUAL_LAT`) | *(empty)* | Decimal degrees, e.g. `40.7128`. Used unless a precise Wi-Fi fix is found (see [Location](#location)) |
| Manual longitude (`FR_MANUAL_LON`) | *(empty)* | Decimal degrees, **west is negative**, e.g. `-74.0060` |
| Wi-Fi geolocation URL (`FR_WIFI_GEO_URL`) | `https://api.beacondb.net/v1/geolocate` | Wi-Fi location service |
| IP geolocation URL (`FR_IP_GEO_URL`) | `https://ipinfo.io/json` | Rough location from the internet address |
| Town-name lookup URL (`FR_REVGEO_URL`) | `https://nominatim.openstreetmap.org/reverse` | OpenStreetMap reverse geocoding |
| ADS-B API base URL (`FR_API_BASE`) | `https://opendata.adsb.fi/api/v3` | Aircraft data. Alternative: `https://api.adsb.lol/v2` |
| Seconds between refreshes (`FR_REFRESH_SEC`) | `10` | 5–120. adsb.fi allows at most 1 request/second |
| Distance units (`FR_UNITS_*`) | Miles | Miles (2/5/10/25) or kilometres (3/8/15/40) |
| OVERHEAD! distance (`FR_OVERHEAD_M`) | `1609` | Metres (1609 = 1 mile). Airborne aircraft closer than this trigger the alert |
| Play a chime (`FR_ALERT_SOUND`) | on | Speaker chime when an aircraft comes overhead |
| Chime volume (`FR_ALERT_VOLUME`) | `60` | 0–100 |
| Vibrate (`FR_ALERT_VIBRATE`) | on | Short buzz with the chime |
| Timezone (`FR_TIMEZONE`) | `EST5EDT,M3.2.0,M11.1.0` | POSIX timezone for the clock, e.g. `CST6CDT,M3.2.0,M11.1.0`, `PST8PDT,M3.2.0,M11.1.0` |

**Finding your coordinates:** in Google Maps, right-click your home and click the numbers at the
top of the menu to copy them (e.g. `40.47751, -74.48262`). The first number is the latitude, the
second the longitude.

**Checking a setting without the menu:**

```bat
findstr FR_ projects\flight_radar\build\sdkconfig
```

**Back to defaults:** delete `projects\flight_radar\build\sdkconfig` and run `system-update` again.
Your settings live in that file, so a full clean or a deleted `build` folder resets them too.

## Location

There is no built-in default location. At every start-up the radar works out where it is,
once, and nothing is saved between boots:

1. **Wi-Fi (precise):** it scans nearby Wi-Fi networks and sends their router MAC
   addresses and signal strengths (never network names) to [beaconDB](https://beacondb.net),
   an open location database. It skips networks whose names end in `_nomap` and randomised MACs.
   It uses the result only if it's precise (within 2 km).
2. **Manual:** otherwise, a location you set yourself is used:
   - on the device: **Vibe Mode > Settings > Shared information**, entry `radar_location`
     (see below). This works for store installs too.
   - or, in your own builds: *Manual latitude/longitude* in menuconfig.

   It's also shown straight away while the Wi-Fi lookup runs.
3. **IP (approximate):** otherwise, it uses [ipinfo.io](https://ipinfo.io), which is city-level
   from your internet address and can be many miles off.

Manual coordinates therefore always win over an approximate fix, but a precise Wi-Fi fix
wins over them. That helps if you move the Mosaico.

It then looks up the town name with one request to OpenStreetMap's
[Nominatim](https://nominatim.org), following its
[usage policy](https://operations.osmfoundation.org/policies/nominatim/): one request per start-up,
with an identifying User-Agent. Place data © OpenStreetMap contributors (ODbL), credited as `OSM` on screen.

The top-left corner shows, for example:

```
SPRINGFIELD
IL 62701
39.799 -89.644
WIFI +-35M        <- or IP (APPROX) / MANUAL
```

### Setting the location on the device (`radar_location`)

Store installs can't use menuconfig: those values are fixed when the app is built. Instead,
the radar reads a value from Vibe Mode's **Shared information**, which every app can read:

1. Hold the **AI** button while powering on to start Vibe Mode.
2. Open **Settings > Shared information** and add an entry. Use the on-screen keyboard:
   - **Key:** `radar_location`
   - **Value:** one of
     - coordinates: `40.4775,-74.4826` (latitude, longitude; west and south are negative)
     - a ZIP/postal code **with the country**: `08902, USA`
     - a place: `North Brunswick, NJ`
3. Restart normally. The corner shows the town it matched and `MANUAL`.

Coordinates are used directly. A ZIP or place name is looked up once per start-up with
OpenStreetMap's Nominatim. Always add the country to a ZIP: a bare `08902`, for example, is
also a postcode in Spain. To go back to automatic, delete the entry. The entry takes priority
over the menuconfig fields, and a precise Wi-Fi fix still takes priority over both.

If it says `IP (APPROX)` and the town is wrong, beaconDB has no Wi-Fi data for your area yet.
Set `radar_location` (or the manual coordinates), and it'll be used instead. If every method fails, the corner shows `NO LOCATION`
and the radar waits.

## Packaging for the ESP-Mosaico store (Ideas)

The store takes a release ZIP built by `mosaico.py project package`. It contains the app's
`.irisfw`, the README, changelog and cover images, and the Idea-grid name, description and
icon in English and Chinese. Everything it needs is in this repo:

| File | Purpose |
|---|---|
| `mosaico-ideas.json` | Store metadata (`mosaico-ideas/project-release/1`): title, categories, tags, file list |
| `CHANGELOG.md` | Release notes shown on the store page |
| `preview.png`, `store/icon.png` | Cover images |
| `store/device/en`, `store/device/zh-CN` | Pre-generated Idea-grid labels + icon, so packaging needs no FreeType/libraqm |
| `CMakeLists.txt` → `project(flight_radar VERSION 1.6.0)` | **The release version.** Bump it here for each store update |

**1. Build the `.irisfw`** (also installs it on the connected board, a good final test):

```bat
C:\esp\espv6.2esp-idf\export.bat
cd C:\esp\esp-mosaico-vibe
python mosaico.py iris system-update --project projects/flight_radar
```

This creates `projects\flight_radar\build\flight_radar-system-update.irisfw`. The `mosaico.py`
toolkit at this workspace's version has no build-only command; `system-update` is how it builds the bundle.

**2. Install the packaging dependencies** (once, into the same Python that runs `mosaico.py`; Pillow is used to check the cover images):

```bat
python -m pip install "jsonschema>=4.23,<5" "markdown-it-py>=3,<5" "Pillow>=10.4,<12"
python -m pip install -r submodule\esp-mosaico-utils\ESP-Iris\components\esp_iris\tools\requirements.txt
```

The second line installs ESP-Iris's tools (e.g. `cryptography`), which check the `.irisfw` inside the package.

**3. Package and validate the store ZIP** (offline):

```bat
python mosaico.py project package --project projects/flight_radar --output dist\flight_radar-1.6.0.zip
python mosaico.py project validate dist\flight_radar-1.6.0.zip --json
```

**4. Upload as a draft** (needs an ESP-Mosaico account). Upload creates a **draft**; publishing is a separate step on the website:

```bat
python mosaico.py account login
python mosaico.py project upload dist\flight_radar-1.6.0.zip --create
```

For later versions, bump the version in `CMakeLists.txt`, add a `CHANGELOG.md` entry, rebuild,
re-package, and use `--update APPLICATION_ID --expected-project-version N` with the ID and draft
version returned by the previous upload.

**Notes:**
- `mosaico-ideas.json` declares the MIT license and links to this repo.
- **Idea-grid labels:** to change the name/description, edit `tools/gen_presentations.py` and rerun it
  on Linux, with Espressif's `shared/ideas` generator, freetype-py and Pillow. The font is Noto Sans SC
  (SIL OFL 1.1) from Vibe Mode's fonts, a subset, so Chinese text is limited to the characters it contains.

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

## License

This project's code is released under the [MIT License](https://github.com/mkulukkallur/esp-mosaico-flight-radar/blob/main/LICENSE).

Parts that come from elsewhere keep their own terms:
- `partitions.csv`, `sdkconfig.defaults`, `sdkconfig.application.defaults`, `tools/quality/` and the
  editor/pre-commit configs come from Espressif's ESP-Mosaico application template
  (`mosaico.py project init`). Espressif hasn't published a license for those template files yet;
  this project doesn't relicense them.
- The Idea-grid labels in `store/device/` are rendered from Noto Sans SC (SIL Open Font License 1.1).
- Data at run time: aircraft from [adsb.fi](https://adsb.fi) (personal, non-commercial use),
  places © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors (ODbL), location
  from [beaconDB](https://beacondb.net) and [ipinfo.io](https://ipinfo.io).

