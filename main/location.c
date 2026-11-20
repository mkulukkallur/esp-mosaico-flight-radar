/*
 * Radar centre, determined once per start-up. Nothing is stored between boots.
 *
 *  1. Wi-Fi: scan nearby access points and ask beaconDB (an open, Mozilla
 *     Location Service compatible database). Only router MAC addresses and
 *     signal strengths are sent; networks named "*_nomap" and randomised
 *     (locally administered) MACs are skipped. Used if precise (<= 2 km).
 *  2. Manual: the "radar_location" entry in Vibe Mode > Settings > Shared
 *     information (coordinates, a ZIP/postal code or a place name), else the
 *     menuconfig latitude/longitude. Overrides any approximate result; shown
 *     immediately while step 1 runs (place names after one geocoding lookup).
 *  3. IP: ipinfo.io's public endpoint (city level, can be miles off).
 *
 * The town name comes from one OpenStreetMap Nominatim reverse lookup per
 * boot, with an identifying User-Agent, per the Nominatim usage policy
 * (https://operations.osmfoundation.org/policies/nominatim/). Data (c)
 * OpenStreetMap contributors, ODbL.
 */
#include <ctype.h>
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
#include "mosaico_shared.h"
#include "radar_ui.h"
#include "wifi.h"

static const char *TAG = "location";

#define USER_AGENT "esp-mosaico-flight-radar/1.3 (+https://github.com/mkulukkallur/esp-mosaico-flight-radar)"
#define MAX_APS 20
#define RESP_MAX 4096
/* A Wi-Fi fix should be within a few hundred metres; anything coarser came
 * from beaconDB's own IP/cell fallback and is reported as IP. */
#define WIFI_ACCURACY_MAX_M 2000
#define SHARED_KEY "radar_location"   /* Vibe Mode > Settings > Shared information */
#define NOMINATIM_GAP_MS 1100         /* Nominatim policy: at most 1 request/second */

static char s_place_query[260];       /* radar_location text that needs geocoding */

const char *location_source_name(loc_source_t src)
{
    switch (src) {
    case LOC_SRC_WIFI:   return "WIFI";
    case LOC_SRC_IP:     return "IP";
    case LOC_SRC_MANUAL: return "MANUAL";
    default:             return "NONE";
    }
}

static bool valid(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180 &&
           !(lat == 0 && lon == 0);
}

static void copy_upper(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i < n - 1; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

/* Earlier builds (v1.2) cached the fix in NVS; remove it so it is never used. */
static void forget_old_cache(void)
{
    nvs_handle_t h;
    if (nvs_open("fradar", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_all(h) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
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
        .user_agent = USER_AGENT,
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
    resp[0] = '\0';
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

/* ---------- 1. Wi-Fi geolocation (beaconDB) ---------- */

static bool locate_wifi(loc_info_t *out, char *resp)
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
    bool ok = false;
    int st = http_request(CONFIG_FR_WIFI_GEO_URL, body, resp, RESP_MAX);
    cJSON_free(body);
    if (st != 200) {
        ESP_LOGW(TAG, "beaconDB returned %d", st);
        return false;
    }
    cJSON *r = cJSON_Parse(resp);
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(r, "location");
    const cJSON *la = cJSON_GetObjectItemCaseSensitive(loc, "lat");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(loc, "lng");
    const cJSON *acc = cJSON_GetObjectItemCaseSensitive(r, "accuracy");
    if (cJSON_IsNumber(la) && cJSON_IsNumber(lo) && valid(la->valuedouble, lo->valuedouble)) {
        out->lat = la->valuedouble;
        out->lon = lo->valuedouble;
        out->accuracy_m = cJSON_IsNumber(acc) ? (int)acc->valuedouble : 0;
        out->src = (out->accuracy_m > 0 && out->accuracy_m <= WIFI_ACCURACY_MAX_M) ? LOC_SRC_WIFI
                                                                                   : LOC_SRC_IP;
        ok = true;
    }
    cJSON_Delete(r);
    return ok;
}

/* ---------- 2. IP geolocation (ipinfo.io) ---------- */

static bool locate_ip(loc_info_t *out, char *resp)
{
    int st = http_request(CONFIG_FR_IP_GEO_URL, NULL, resp, RESP_MAX);
    if (st != 200) {
        ESP_LOGW(TAG, "ipinfo returned %d", st);
        return false;
    }
    bool ok = false;
    cJSON *r = cJSON_Parse(resp);
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(r, "loc"); /* "40.73,-74.17" */
    if (cJSON_IsString(loc) && loc->valuestring) {
        char *end;
        double a = strtod(loc->valuestring, &end);
        if (*end == ',') {
            double b = strtod(end + 1, NULL);
            if (valid(a, b)) {
                out->lat = a;
                out->lon = b;
                out->src = LOC_SRC_IP;
                out->accuracy_m = 0;
                /* Fallback names, replaced by the reverse lookup if it works. */
                const cJSON *city = cJSON_GetObjectItemCaseSensitive(r, "city");
                const cJSON *postal = cJSON_GetObjectItemCaseSensitive(r, "postal");
                if (cJSON_IsString(city)) {
                    copy_upper(out->place, sizeof(out->place), city->valuestring);
                }
                if (cJSON_IsString(postal)) {
                    copy_upper(out->postcode, sizeof(out->postcode), postal->valuestring);
                }
                ok = true;
            }
        }
    }
    cJSON_Delete(r);
    return ok;
}

/* ---------- town name (OpenStreetMap Nominatim) ---------- */

/* Town / state / ZIP from a Nominatim "address" object. */
static void fill_address(loc_info_t *info, const cJSON *addr)
{
    if (!cJSON_IsObject(addr)) {
        return;
    }
    static const char *const keys[] = {"town", "city", "village", "hamlet",
                                       "suburb", "municipality", "county"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(addr, keys[i]);
        if (cJSON_IsString(v) && v->valuestring[0]) {
            copy_upper(info->place, sizeof(info->place), v->valuestring);
            break;
        }
    }
    const cJSON *pc = cJSON_GetObjectItemCaseSensitive(addr, "postcode");
    if (cJSON_IsString(pc)) {
        copy_upper(info->postcode, sizeof(info->postcode), pc->valuestring);
    }
    /* "ISO3166-2-lvl4": "US-NJ" -> "NJ" */
    const cJSON *iso = cJSON_GetObjectItemCaseSensitive(addr, "ISO3166-2-lvl4");
    if (cJSON_IsString(iso)) {
        const char *dash = strchr(iso->valuestring, '-');
        copy_upper(info->region, sizeof(info->region), dash ? dash + 1 : iso->valuestring);
    }
}

static void reverse_geocode(loc_info_t *info, char *resp)
{
    char url[256];
    snprintf(url, sizeof(url), "%s?format=jsonv2&lat=%.5f&lon=%.5f&zoom=14&addressdetails=1&accept-language=en",
             CONFIG_FR_REVGEO_URL, info->lat, info->lon);
    int st = http_request(url, NULL, resp, RESP_MAX);
    if (st != 200) {
        ESP_LOGW(TAG, "Nominatim returned %d", st);
        return;
    }
    cJSON *r = cJSON_Parse(resp);
    fill_address(info, cJSON_GetObjectItemCaseSensitive(r, "address"));
    cJSON_Delete(r);
}

/* Place name or ZIP -> coordinates + names (one Nominatim search). */
static bool geocode_place(const char *query, loc_info_t *info, char *resp)
{
    char q[3 * sizeof(s_place_query)];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p && o + 4 < sizeof(q); p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) {
            q[o++] = (char)*p;
        } else {
            o += snprintf(q + o, sizeof(q) - o, "%%%02X", *p);
        }
    }
    q[o] = '\0';
    char url[1024];
    snprintf(url, sizeof(url), "%s?format=jsonv2&limit=1&addressdetails=1&accept-language=en&q=%s",
             CONFIG_FR_GEOCODE_URL, q);
    int st = http_request(url, NULL, resp, RESP_MAX);
    if (st != 200) {
        ESP_LOGW(TAG, "Nominatim search returned %d", st);
        return false;
    }
    bool ok = false;
    cJSON *r = cJSON_Parse(resp);
    const cJSON *first = cJSON_IsArray(r) ? cJSON_GetArrayItem(r, 0) : NULL;
    const cJSON *la = cJSON_GetObjectItemCaseSensitive(first, "lat");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(first, "lon");
    if (cJSON_IsString(la) && cJSON_IsString(lo)) {
        double lat = strtod(la->valuestring, NULL), lon = strtod(lo->valuestring, NULL);
        if (valid(lat, lon)) {
            info->lat = lat;
            info->lon = lon;
            info->src = LOC_SRC_MANUAL;
            fill_address(info, cJSON_GetObjectItemCaseSensitive(first, "address"));
            ok = true;
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "No match for \"%s\"", query);
    }
    cJSON_Delete(r);
    return ok;
}

/* ---------- task ---------- */

/* "40.4775,-74.4826", "40.4775 -74.4826" or "40.4775, -74.4826" */
static bool parse_coords(const char *s, double *lat, double *lon)
{
    char *end;
    double a = strtod(s, &end);
    if (end == s) {
        return false;
    }
    while (*end == ' ' || *end == ',') {
        end++;
    }
    const char *second = end;
    double b = strtod(second, &end);
    if (end == second) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end || !valid(a, b)) {
        return false;
    }
    *lat = a;
    *lon = b;
    return true;
}

static bool manual_location(loc_info_t *info)
{
    /* 1. Vibe Mode > Settings > Shared information: radar_location */
    char v[sizeof(s_place_query)];
    if (mosaico_shared_get(SHARED_KEY, v, sizeof(v)) == ESP_OK) {
        char *t = v;
        while (*t == ' ') t++;
        size_t n = strlen(t);
        while (n && t[n - 1] == ' ') t[--n] = '\0';
        if (*t) {
            if (parse_coords(t, &info->lat, &info->lon)) {
                ESP_LOGI(TAG, "Using %s from Vibe Mode shared information", SHARED_KEY);
                info->src = LOC_SRC_MANUAL;
                return true;
            }
            ESP_LOGI(TAG, "%s \"%s\" will be looked up", SHARED_KEY, t);
            strlcpy(s_place_query, t, sizeof(s_place_query));
            return false;
        }
    }
    /* 2. menuconfig (developer builds) */
    if (!CONFIG_FR_MANUAL_LAT[0] || !CONFIG_FR_MANUAL_LON[0]) {
        return false;
    }
    double lat = strtod(CONFIG_FR_MANUAL_LAT, NULL);
    double lon = strtod(CONFIG_FR_MANUAL_LON, NULL);
    if (!valid(lat, lon)) {
        ESP_LOGW(TAG, "Ignoring invalid manual location \"%s\", \"%s\"", CONFIG_FR_MANUAL_LAT,
                 CONFIG_FR_MANUAL_LON);
        return false;
    }
    info->lat = lat;
    info->lon = lon;
    info->src = LOC_SRC_MANUAL;
    return true;
}

static void location_task(void *arg)
{
    (void)arg;
    loc_info_t info, manual;
    memset(&info, 0, sizeof(info));
    memset(&manual, 0, sizeof(manual));
    forget_old_cache();

    /* Manual coordinates (if set) start the radar right away, before any network. */
    bool have_manual = manual_location(&manual);
    if (have_manual) {
        adsb_set_home(manual.lat, manual.lon);
        radar_ui_set_location(&manual);
    }

    wifi_wait_connected(portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let DHCP/DNS settle */

    char *resp = malloc(RESP_MAX);
    if (!resp) {
        vTaskDelete(NULL);
        return;
    }

    /* A place name / ZIP in radar_location: look it up once. */
    bool names_known = false, nominatim_used = false;
    if (!have_manual && s_place_query[0]) {
        nominatim_used = true;
        if (geocode_place(s_place_query, &manual, resp)) {
            have_manual = names_known = true;
            ESP_LOGI(TAG, "\"%s\" -> %.5f, %.5f", s_place_query, manual.lat, manual.lon);
            adsb_set_home(manual.lat, manual.lon);
            radar_ui_set_location(&manual);
        }
    }

    /* Precedence: precise Wi-Fi fix > manual coordinates > approximate (IP) fix. */
    bool have = locate_wifi(&info, resp);
    if (have && info.src == LOC_SRC_WIFI) {
        ESP_LOGI(TAG, "Precise Wi-Fi fix (%d m)%s", info.accuracy_m,
                 have_manual ? "; overriding manual coordinates" : "");
    } else if (have_manual) {
        ESP_LOGI(TAG, "No precise Wi-Fi fix; keeping manual coordinates");
        info = manual;
        have = true;
    } else if (!have) {
        have = locate_ip(&info, resp);
    }

    if (!have) {
        ESP_LOGE(TAG, "Automatic location failed; set coordinates in menuconfig");
        info.failed = true;
        radar_ui_set_location(&info);
        free(resp);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Centre via %s: %.5f, %.5f (accuracy %d m)", location_source_name(info.src),
             info.lat, info.lon, info.accuracy_m);
    if (info.src != LOC_SRC_MANUAL) {
        adsb_set_home(info.lat, info.lon); /* manual was already applied */
        radar_ui_set_location(&info);      /* show coordinates while the name loads */
    }

    if (!(info.src == LOC_SRC_MANUAL && names_known)) {
        if (nominatim_used) {
            vTaskDelay(pdMS_TO_TICKS(NOMINATIM_GAP_MS));
        }
        reverse_geocode(&info, resp);
    }
    ESP_LOGI(TAG, "Place: %s %s %s", info.place, info.region, info.postcode);
    radar_ui_set_location(&info);
    free(resp);
    vTaskDelete(NULL);
}

void location_start(void)
{
    xTaskCreate(location_task, "location", 10 * 1024, NULL, 3, NULL);
}
