/*
 * Live aircraft from an ADSBexchange-v2-compatible API (default: adsb.fi open data v3).
 *   GET {base}/lat/{lat}/lon/{lon}/dist/{nm}
 * Accepts replies with either an "ac" or an "aircraft" array.
 * Response: { "ac": [ { "hex", "flight", "lat", "lon", "alt_baro", "gs", "track", ... } ] }
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "adsb.h"
#include "radar_math.h"
#include "wifi.h"

static const char *TAG = "adsb";

#define BUF_START (64 * 1024)
#define BUF_MAX   (1024 * 1024)
#define READ_CHUNK 4096

static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static adsb_snapshot_t s_shared; /* guarded by s_mtx */
static adsb_snapshot_t s_work;   /* fetch task only */
static double s_lat, s_lon;           /* fetch task copy */
static double s_home_lat, s_home_lon; /* guarded by s_mtx */
static volatile double s_radius_km = 16;
static char *s_buf;
static size_t s_cap;

static void *buf_realloc(void *p, size_t n)
{
    void *r = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) {
        r = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
    }
    return r;
}

/* Returns HTTP status (200 on success) or a negative error. Body in s_buf. */
static int http_get(const char *url, size_t *out_len)
{
    *out_len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = READ_CHUNK,
        .buffer_size_tx = 1024,
        .user_agent = "esp-mosaico-flight-radar/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    esp_http_client_set_header(c, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return -2;
    }
    (void)esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    size_t len = 0;
    if (status == 200) {
        for (;;) {
            if (s_cap - len < READ_CHUNK + 1) {
                if (s_cap >= BUF_MAX) {
                    status = -3; /* response too large */
                    break;
                }
                size_t ncap = s_cap ? s_cap * 2 : BUF_START;
                if (ncap > BUF_MAX) {
                    ncap = BUF_MAX;
                }
                char *nb = buf_realloc(s_buf, ncap);
                if (!nb) {
                    status = -4; /* out of memory */
                    break;
                }
                s_buf = nb;
                s_cap = ncap;
            }
            int n = esp_http_client_read(c, s_buf + len, s_cap - len - 1);
            if (n < 0) {
                status = -5; /* read error / timeout */
                break;
            }
            if (n == 0) {
                break;
            }
            len += n;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (s_buf) {
        s_buf[len] = '\0';
    }
    *out_len = len;
    return status;
}

static void copy_trim_upper(char *dst, size_t n, const char *src)
{
    while (*src == ' ') {
        src++;
    }
    size_t i = 0;
    for (; src[i] && i < n - 1; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
    while (i > 0 && dst[i - 1] == ' ') {
        dst[--i] = '\0';
    }
}

static bool get_num(const cJSON *o, const char *key, double *v)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(it)) {
        *v = it->valuedouble;
        return true;
    }
    return false;
}

static const char *get_str(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

/* Keep the ADSB_MAX_AIRCRAFT nearest aircraft, sorted by distance. */
static void insert_nearest(adsb_snapshot_t *s, const aircraft_t *a)
{
    int n = s->count;
    if (n == ADSB_MAX_AIRCRAFT && a->dist_km >= s->ac[n - 1].dist_km) {
        return;
    }
    int pos = (n < ADSB_MAX_AIRCRAFT) ? n : n - 1;
    while (pos > 0 && s->ac[pos - 1].dist_km > a->dist_km) {
        s->ac[pos] = s->ac[pos - 1];
        pos--;
    }
    s->ac[pos] = *a;
    if (n < ADSB_MAX_AIRCRAFT) {
        s->count++;
    }
}

static bool parse(const char *json, size_t len, adsb_snapshot_t *s)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "ac");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItemCaseSensitive(root, "aircraft");
    }

    s->count = 0;
    s->total = 0;
    const cJSON *it;
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            double lat, lon, v;
            if (!get_num(it, "lat", &lat) || !get_num(it, "lon", &lon)) {
                continue; /* no position, can't plot */
            }
            aircraft_t a;
            memset(&a, 0, sizeof(a));
            a.lat = lat;
            a.lon = lon;

            const char *str;
            if ((str = get_str(it, "hex"))) {
                copy_trim_upper(a.hex, sizeof(a.hex), str);
            }
            if ((str = get_str(it, "flight"))) {
                copy_trim_upper(a.callsign, sizeof(a.callsign), str);
            }
            if (!a.callsign[0] && (str = get_str(it, "r"))) {
                copy_trim_upper(a.callsign, sizeof(a.callsign), str);
            }
            if (!a.callsign[0]) {
                strlcpy(a.callsign, a.hex[0] ? a.hex : "?", sizeof(a.callsign));
            }
            if ((str = get_str(it, "t"))) {
                copy_trim_upper(a.type, sizeof(a.type), str);
            }

            const cJSON *alt = cJSON_GetObjectItemCaseSensitive(it, "alt_baro");
            if (cJSON_IsNumber(alt)) {
                a.alt_ft = (int)alt->valuedouble;
                a.has_alt = true;
            } else if (cJSON_IsString(alt) && alt->valuestring && strcmp(alt->valuestring, "ground") == 0) {
                a.on_ground = true;
                a.has_alt = true;
            } else if (get_num(it, "alt_geom", &v)) {
                a.alt_ft = (int)v;
                a.has_alt = true;
            }
            if (get_num(it, "gs", &v)) {
                a.gs_kt = (float)v;
                a.has_gs = true;
            }
            if (get_num(it, "track", &v) || get_num(it, "true_heading", &v) || get_num(it, "mag_heading", &v)) {
                a.track_deg = (float)v;
                a.has_track = true;
            }

            a.dist_km = geo_distance_km(s_lat, s_lon, lat, lon);
            a.bearing_deg = (float)geo_bearing_deg(s_lat, s_lon, lat, lon);
            s->total++;
            insert_nearest(s, &a);
        }
    }
    cJSON_Delete(root);
    return true;
}

static void fetch_task(void *arg)
{
    char url[192];
    for (;;) {
        wifi_wait_connected(portMAX_DELAY);

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_lat = s_home_lat;
        s_lon = s_home_lon;
        xSemaphoreGive(s_mtx);

        /* Query a bit beyond the displayed range so edge traffic shows up. */
        int nm = (int)ceil(s_radius_km * 1.25 / KM_PER_NM);
        if (nm < 1) nm = 1;
        if (nm > 250) nm = 250;
        snprintf(url, sizeof(url), "%s/lat/%.4f/lon/%.4f/dist/%d", CONFIG_FR_API_BASE, s_lat, s_lon, nm);

        size_t len = 0;
        int status = http_get(url, &len);
        bool ok = (status == 200) && parse(s_buf, len, &s_work);
        if (status == 200 && !ok) {
            status = -10; /* bad JSON */
        }

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        uint32_t seq = s_shared.seq;
        if (ok) {
            s_shared = s_work;
            s_shared.fetched_us = esp_timer_get_time();
        }
        s_shared.ok = ok;
        s_shared.status = status;
        s_shared.seq = seq + 1;
        xSemaphoreGive(s_mtx);

        if (ok) {
            ESP_LOGI(TAG, "%d aircraft within %d NM (%u bytes)", s_work.total, nm, (unsigned)len);
        } else {
            ESP_LOGW(TAG, "fetch failed (%d): %s", status, url);
        }

        /* Sleep until next refresh, or wake early if the range changed. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? CONFIG_FR_REFRESH_SEC * 1000 : 5000));
    }
}

void adsb_start(double home_lat, double home_lon, double radius_km)
{
    s_lat = s_home_lat = home_lat;
    s_lon = s_home_lon = home_lon;
    s_radius_km = radius_km;
    s_mtx = xSemaphoreCreateMutex();
    xTaskCreate(fetch_task, "adsb", 12 * 1024, NULL, 4, &s_task);
}

void adsb_set_home(double lat, double lon)
{
    if (!s_mtx) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_home_lat = lat;
    s_home_lon = lon;
    xSemaphoreGive(s_mtx);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void adsb_set_radius_km(double radius_km)
{
    s_radius_km = radius_km;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool adsb_get_snapshot(adsb_snapshot_t *out, uint32_t last_seq)
{
    if (!s_mtx || xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    bool changed = s_shared.seq != last_seq;
    if (changed) {
        *out = s_shared;
    }
    xSemaphoreGive(s_mtx);
    return changed;
}
