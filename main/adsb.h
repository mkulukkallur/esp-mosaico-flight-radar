#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADSB_MAX_AIRCRAFT 48

typedef struct {
    char hex[8];        /* ICAO 24-bit address, upper-case */
    char callsign[10];  /* flight / registration / hex fallback */
    char type[6];       /* e.g. B738 */
    double lat, lon;
    double dist_km;     /* from home */
    float bearing_deg;  /* from home, 0 = north */
    int alt_ft;
    float gs_kt;
    float track_deg;
    bool has_alt, on_ground, has_gs, has_track;
} aircraft_t;

typedef struct {
    aircraft_t ac[ADSB_MAX_AIRCRAFT]; /* sorted nearest first */
    int count;                        /* entries in ac[] */
    int total;                        /* aircraft with a position in the API reply */
    bool ok;                          /* last fetch succeeded */
    int status;                       /* HTTP status or negative error of last fetch */
    int64_t fetched_us;               /* esp_timer time of last successful fetch */
    uint32_t seq;                     /* increments after every fetch attempt */
} adsb_snapshot_t;

/** Start the background fetch task. */
void adsb_start(double home_lat, double home_lon, double radius_km);

/** Re-centre the radar on a new home position (wakes the fetch task). Any task. */
void adsb_set_home(double lat, double lon);

/** Change the query radius (wakes the fetch task for an immediate refresh). */
void adsb_set_radius_km(double radius_km);

/** Copy the latest snapshot if its seq differs from last_seq. Safe from any task. */
bool adsb_get_snapshot(adsb_snapshot_t *out, uint32_t last_seq);

#ifdef __cplusplus
}
#endif
