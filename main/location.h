#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOC_SRC_NONE = 0,   /* not located (yet, or failed) */
    LOC_SRC_MANUAL,     /* menuconfig coordinates */
    LOC_SRC_WIFI,       /* nearby Wi-Fi networks (beaconDB) */
    LOC_SRC_IP,         /* internet address: approximate, city level */
} loc_source_t;

typedef struct {
    loc_source_t src;
    bool failed;        /* true when every method failed */
    double lat, lon;
    int accuracy_m;     /* 0 = unknown */
    char place[24];     /* town / city, upper-case, may be empty */
    char region[8];     /* e.g. "NJ" */
    char postcode[12];  /* e.g. ZIP code */
} loc_info_t;

/**
 * Work out the radar centre once, at start-up, in a background task.
 * Precedence: precise Wi-Fi fix (beaconDB) > menuconfig coordinates >
 * approximate internet-address fix (ipinfo.io). Then looks up the town name (OpenStreetMap
 * Nominatim, one request) and hands everything to adsb_set_home() and
 * radar_ui_set_location(). Nothing is saved between boots.
 */
void location_start(void);

/** Short label for a source, e.g. "WIFI". */
const char *location_source_name(loc_source_t src);

#ifdef __cplusplus
}
#endif
