# No REQUIRES list: "main" may use every component in the build (BSP, LVGL,
# ESP-Iris, recovery support, cJSON), which avoids managed-component naming.
idf_component_register(
    SRCS "main.c" "wifi.c" "adsb.c" "radar_math.c" "radar_ui.c" "location.c"
    INCLUDE_DIRS ".")
