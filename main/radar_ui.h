#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Build the radar screen. Call once with the LVGL lock held. */
void radar_ui_create(void);

/** Show "no Wi-Fi configured" in the footer (safe from any task). */
void radar_ui_set_no_wifi(void);

#include "location.h"

/** Show the radar centre (place, ZIP, coordinates, source) in the top-left
 *  corner. Safe from any task; the struct is copied. */
void radar_ui_set_location(const loc_info_t *info);

/** Currently selected display range, in km. */
double radar_ui_range_km(void);

#ifdef __cplusplus
}
#endif
