#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EARTH_RADIUS_KM 6371.0
#define KM_PER_NM       1.852
#define KM_PER_MI       1.609344

/** Wrap any angle into [0, 360). */
double wrap360(double deg);

/** Great-circle distance between two lat/lon points, in km. */
double geo_distance_km(double lat1, double lon1, double lat2, double lon2);

/** Initial compass bearing from point 1 to point 2 (0 = north, clockwise). */
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/**
 * Project a (distance, bearing) onto the radar.
 * Returns pixel offsets from the radar centre (x right, y down).
 * Anything beyond `range` is clamped onto the outer ring.
 */
void radar_project(double dist, double bearing_deg, double range, int radius_px, int *dx, int *dy);

/** Degrees the sweep has travelled since it passed `target` (0..360). */
double sweep_since(double sweep_deg, double target_deg);

#ifdef __cplusplus
}
#endif
