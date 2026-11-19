#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Build the radar screen. Call once with the LVGL lock held. */
void radar_ui_create(void);

/** Show "no Wi-Fi configured" in the footer (safe from any task). */
void radar_ui_set_no_wifi(void);

/** Show where the radar centre came from for a while (safe from any task).
 *  src is a loc_source_t; accuracy_m < 0 means automatic location failed. */
void radar_ui_set_location(int src, int accuracy_m);

/** Currently selected display range, in km. */
double radar_ui_range_km(void);

#ifdef __cplusplus
}
#endif
