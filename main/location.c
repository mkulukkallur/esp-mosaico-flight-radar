/*
 * Automatic location for the radar centre.
 *
 *  1. Wi-Fi: scan nearby access points and ask beaconDB (an open, Mozilla
 *     Location Service compatible database). Only router MAC addresses and
 *     signal strengths are sent; networks marked "_nomap" and randomised
 *     (locally administered) MACs are skipped.
 *  2. IP: if that fails, ipinfo.io's public endpoint (city-level, no account).
 *  3. Otherwise keep the last saved fix, or the menuconfig coordinates.
 *
 * A successful fix is saved in NVS so the next boot starts in the right place.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "adsb.h"
#include "location.h"
#include "radar_ui.h"
#include "wifi.h"

static const char *TAG = "location";

#define NVS_NS "fradar"
#define MAX_APS 20
#define RESP_MAX 2048
/* A Wi-Fi fix should be within a few hundred metres; anything coarser than
 * this came from beaconDB's IP/cell fallback and is reported as IP. */
#define WIFI_ACCURACY_MAX_M 2000

const char *location_source_name(loc_source_t src)
{
    switch (src) {
    case LOC_SRC_WIFI: return "WIFI";
    case LOC_SRC_IP:   return "IP";
    case LOC_SRC_SAVED: return "SAVED";
    default:           return "MANUAL";
    }
}

static bool valid(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180 &&
           !(lat == 0 && lon == 0);
}

/* ---------- NVS cache ---------- */

static bool load_saved(double *lat, double *lon)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    double v[2];
    size_t len = sizeof(v);
    bool ok = nvs_get_blob(h, "home", v, &len) == ESP_OK && len == sizeof(v) && valid(v[0], v[1]);
    nvs_close(h);
    if (ok) {
        *lat = v[0];
        *lon = v[1];
    }
    return ok;
}

static void save(double lat, double lon)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    double v[2] = {lat, lon};
    if (nvs_set_blob(h, "home", v, sizeof(v)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

loc_source_t location_initial(double *lat, double *lon)
{
    *lat = strtod(CONFIG_FR_HOME_LAT, NULL);
    *lon = strtod(CONFIG_FR_HOME_LON, NULL);
#if CONFIG_FR_LOCATION_AUTO
    if (load_saved(lat, lon)) {
        return LOC_SRC_SAVED;
    }
#endif
    return LOC_SRC_MANUAL;
}

/* ---------- small HTTPS helper ---------- */

/* GET (body == NULL) or POST JSON. Returns HTTP status or negative error. */
static int http_request(const char *url, const char *body, char *resp, size_t resp_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "esp-mosaico-flight-radar/1.2",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    esp_http_client_set_header(c, "Accept", "application/json");
    int body_len = body ? (int)strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    int status = -2;
    if (esp_http_client_open(c, body_len) == ESP_OK) {
        if (!body || esp_http_client_write(c, body, body_len) == body_len) {
            (void)esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            int total = 0, n;
            while (total < (int)resp_len - 1 &&
                   (n = esp_http_client_read(c, resp + total, resp_len - 1 - total)) > 0) {
                total += n;
            }
            resp[total] = '\0';
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return status;
}

/* ---------- Wi-Fi geolocation (beaconDB) ---------- */

static bool locate_wifi(double *lat, double *lon, int *accuracy_m)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed");
        return false;
    }
    uint16_t n = MAX_APS;
    wifi_ap_record_t *aps = calloc(MAX_APS, sizeof(wifi_ap_record_t));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return false;
    }
    esp_wifi_scan_get_ap_records(&n, aps);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "considerIp", true);
    cJSON *arr = cJSON_AddArrayToObject(root, "wifiAccessPoints");
    int used = 0;
    for (int i = 0; i < n; i++) {
        const wifi_ap_record_t *ap = &aps[i];
        if (ap->bssid[0] & 0x02) {
            continue; /* locally administered / randomised MAC */
        }
        const char *ssid = (const char *)ap->ssid;
        size_t sl = strlen(ssid);
        if (sl >= 6 && strcmp(ssid + sl - 6, "_nomap") == 0) {
            continue; /* owner opted out of location databases */
        }
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", ap->bssid[0], ap->bssid[1],
                 ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "macAddress", mac);
        cJSON_AddNumberToObject(o, "signalStrength", ap->rssi);
        cJSON_AddNumberToObject(o, "channel", ap->primary);
        cJSON_AddItemToArray(arr, o);
        used++;
    }
    free(aps);
    ESP_LOGI(TAG, "Wi-Fi scan: %d networks usable", used);
    if (used < 2) {
        cJSON_Delete(root);
        return false;
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }
    char *resp = malloc(RESP_MAX);
    bool ok = false;
    if (resp) {
        int st = http_request(CONFIG_FR_WIFI_GEO_URL, body, resp, RESP_MAX);
        if (st == 200) {
            cJSON *r = cJSON_Parse(resp);
            const cJSON *loc = cJSON_GetObjectItemCaseSensitive(r, "location");
            const cJSON *la = cJSON_GetObjectItemCaseSensitive(loc, "lat");
            const cJSON *lo = cJSON_GetObjectItemCaseSensitive(loc, "lng");
            const cJSON *acc = cJSON_GetObjectItemCaseSensitive(r, "accuracy");
            if (cJSON_IsNumber(la) && cJSON_IsNumber(lo) && valid(la->valuedouble, lo->valuedouble)) {
                *lat = la->valuedouble;
                *lon = lo->valuedouble;
                *accuracy_m = cJSON_IsNumber(acc) ? (int)acc->valuedouble : 0;
                ok = true;
            }
            cJSON_Delete(r);
        } else {
            ESP_LOGW(TAG, "beaconDB returned %d", st);
        }
        free(resp);
    }
    cJSON_free(body);
    return ok;
}

/* ---------- IP geolocation (ipinfo.io) ---------- */

static bool locate_ip(double *lat, double *lon)
{
    char *resp = malloc(RESP_MAX);
    if (!resp) {
        return false;
    }
    bool ok = false;
    int st = http_request(CONFIG_FR_IP_GEO_URL, NULL, resp, RESP_MAX);
    if (st == 200) {
        cJSON *r = cJSON_Parse(resp);
        const cJSON *loc = cJSON_GetObjectItemCaseSensitive(r, "loc"); /* "40.44,-74.48" */
        if (cJSON_IsString(loc) && loc->valuestring) {
            char *end;
            double a = strtod(loc->valuestring, &end);
            if (*end == ',') {
                double b = strtod(end + 1, NULL);
                if (valid(a, b)) {
                    *lat = a;
                    *lon = b;
                    ok = true;
                }
            }
        }
        cJSON_Delete(r);
    } else {
        ESP_LOGW(TAG, "ipinfo returned %d", st);
    }
    free(resp);
    return ok;
}

/* ---------- task ---------- */

static void location_task(void *arg)
{
    (void)arg;
    wifi_wait_connected(portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let DHCP/DNS settle */

    double lat, lon;
    int acc = 0;
    loc_source_t src;
    if (locate_wifi(&lat, &lon, &acc)) {
        src = (acc > 0 && acc <= WIFI_ACCURACY_MAX_M) ? LOC_SRC_WIFI : LOC_SRC_IP;
    } else if (locate_ip(&lat, &lon)) {
        src = LOC_SRC_IP;
        acc = 0;
    } else {
        ESP_LOGW(TAG, "Automatic location failed; keeping current centre");
        radar_ui_set_location(LOC_SRC_MANUAL, -1);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Located via %s: %.4f, %.4f (accuracy %d m)", location_source_name(src), lat, lon, acc);
    save(lat, lon);
    adsb_set_home(lat, lon);
    radar_ui_set_location(src, acc);
    vTaskDelete(NULL);
}

void location_start_auto(void)
{
#if CONFIG_FR_LOCATION_AUTO
    xTaskCreate(location_task, "location", 10 * 1024, NULL, 3, NULL);
#endif
}
