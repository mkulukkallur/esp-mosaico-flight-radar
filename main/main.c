/*
 * ESP-Mosaico Flight Radar
 * Live ADS-B aircraft around your location on a green radar display.
 * Data: adsb.fi open data (personal, non-commercial use).
 *
 * Runs as a normal Mosaico application next to Vibe Mode: ESP-Iris and the
 * recovery support start first, so the board can always return to Vibe Mode
 * and accept updates even if the radar fails later.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "iris_ota_support.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "adsb.h"
#include "location.h"
#include "radar_ui.h"
#include "wifi.h"

static const char *TAG = "flight_radar";

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Vibe Mode / update support must stay reachable whatever happens next. */
    iris_ota_support_start();

    /* Larger LVGL task stack than the adapter default: the radar redraws many
     * objects and formats text inside LVGL timers and input callbacks. */
    bsp_display_config_t disp_cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    disp_cfg.task_stack_size = 16 * 1024;
    lv_display_t *display = bsp_display_start_with_config(&disp_cfg);
    if (!display) {
        ESP_LOGE(TAG, "Display unavailable; Vibe Mode RPC remains active");
        return;
    }
    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return;
    }
    radar_ui_create();
    bsp_display_unlock();

    /* Wi-Fi: menuconfig override if set, otherwise the network saved in Vibe Mode. */
    static char ssid[33], password[65];
    if (CONFIG_FR_WIFI_SSID[0]) {
        strlcpy(ssid, CONFIG_FR_WIFI_SSID, sizeof(ssid));
        strlcpy(password, CONFIG_FR_WIFI_PASSWORD, sizeof(password));
        ESP_LOGI(TAG, "Using Wi-Fi from menuconfig");
    } else if (wifi_load_vibe_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Using Wi-Fi saved in Vibe Mode");
    } else {
        ESP_LOGE(TAG, "No Wi-Fi configured: set it up in Vibe Mode or in menuconfig");
        radar_ui_set_no_wifi();
        return;
    }
    wifi_start(ssid, password);

    /* Clock in the header */
    setenv("TZ", CONFIG_FR_TIMEZONE, 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    adsb_start(radar_ui_range_km()); /* waits for the location */
    location_start();                /* works out the location once, now */
    ESP_LOGI(TAG, "Flight radar running");
}
