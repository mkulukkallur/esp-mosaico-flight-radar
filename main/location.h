#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOC_SRC_MANUAL = 0, /* menuconfig coordinates */
    LOC_SRC_SAVED,      /* last automatic fix, from NVS */
    LOC_SRC_WIFI,       /* nearby Wi-Fi networks (beaconDB) */
    LOC_SRC_IP,         /* internet address (ipinfo.io / beaconDB IP fallback) */
} loc_source_t;

/**
 * Best location available right now, without network access:
 * the last saved automatic fix if there is one, else the menuconfig coordinates.
 */
loc_source_t location_initial(double *lat, double *lon);

/**
 * Start a background task that waits for Wi-Fi, works out the location
 * (Wi-Fi scan -> beaconDB, then IP -> ipinfo.io), saves it, and re-centres
 * the radar via adsb_set_home(). Does nothing if auto-location is disabled.
 */
void location_start_auto(void);

/** Short label for a source, e.g. "WIFI". */
const char *location_source_name(loc_source_t src);

#ifdef __cplusplus
}
#endif
