#include <math.h>
#include "radar_math.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((d) * M_PI / 180.0)
#define RAD2DEG(r) ((r) * 180.0 / M_PI)

double wrap360(double deg)
{
    deg = fmod(deg, 360.0);
    if (deg < 0) {
        deg += 360.0;
    }
    return deg;
}

double geo_distance_km(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = DEG2RAD(lat1), p2 = DEG2RAD(lat2);
    double dp = DEG2RAD(lat2 - lat1), dl = DEG2RAD(lon2 - lon1);
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2.0 * EARTH_RADIUS_KM * atan2(sqrt(a), sqrt(1.0 - a));
}

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = DEG2RAD(lat1), p2 = DEG2RAD(lat2), dl = DEG2RAD(lon2 - lon1);
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    return wrap360(RAD2DEG(atan2(y, x)));
}

void radar_project(double dist, double bearing_deg, double range, int radius_px, int *dx, int *dy)
{
    double r = (range > 0) ? dist / range * radius_px : 0;
    if (r > radius_px) {
        r = radius_px;
    }
    *dx = (int)lround(r * sin(DEG2RAD(bearing_deg)));
    *dy = (int)lround(-r * cos(DEG2RAD(bearing_deg)));
}

double sweep_since(double sweep_deg, double target_deg)
{
    return wrap360(sweep_deg - target_deg);
}
