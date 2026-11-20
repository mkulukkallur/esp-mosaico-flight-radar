/*
 * "Aircraft overhead" alert: a short rising two-note chime on the Mosaico
 * speaker plus a brief buzz of the vibration motor.
 *
 * Audio set-up follows the official esp-mosaico-bsp dual_screen_pong example:
 * speaker-only I2S (microphone input left unassigned), 16 kHz mono, 16-bit.
 * Tones are generated on the fly, so no sound files are needed.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "alert.h"

static const char *TAG = "alert";

#define SAMPLE_RATE 16000
#define CHUNK 160          /* 10 ms */
#define RAMP 80            /* 5 ms fade in/out, avoids clicks */
#define AMPLITUDE 9000     /* of 32767; volume is set on the codec */

static QueueHandle_t s_queue;
static esp_codec_dev_handle_t s_speaker;
static bool s_audio_ok, s_motor_ok;

static void init_speaker(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = GPIO_NUM_NC,
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    esp_err_t err = bsp_audio_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sound off: I2S init failed (%s)", esp_err_to_name(err));
        return;
    }
    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        ESP_LOGW(TAG, "Sound off: speaker codec init failed");
        return;
    }
    esp_codec_dev_sample_info_t info = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    int ret = esp_codec_dev_open(s_speaker, &info);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Sound off: speaker open failed (%d)", ret);
        return;
    }
    esp_codec_dev_set_out_vol(s_speaker, CONFIG_FR_ALERT_VOLUME);
    s_audio_ok = true;
}

/* One sine note with a short fade at each end. */
static void play_note(float freq_hz, int duration_ms)
{
    int16_t pcm[CHUNK];
    const int total = duration_ms * SAMPLE_RATE / 1000;
    const float step = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE;
    float phase = 0;
    for (int done = 0; done < total && s_audio_ok;) {
        int n = (total - done) < CHUNK ? (total - done) : CHUNK;
        for (int i = 0; i < n; i++) {
            int idx = done + i;
            int env = RAMP;
            if (idx < env) env = idx;
            if (total - 1 - idx < env) env = total - 1 - idx;
            pcm[i] = (int16_t)(sinf(phase) * AMPLITUDE * env / RAMP);
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        if (esp_codec_dev_write(s_speaker, pcm, n * (int)sizeof(pcm[0])) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Sound write failed; disabling sound");
            s_audio_ok = false;
        }
        done += n;
    }
}

static void silence(int duration_ms)
{
    int16_t zero[CHUNK] = {0};
    for (int left = duration_ms * SAMPLE_RATE / 1000; left > 0 && s_audio_ok; left -= CHUNK) {
        int n = left < CHUNK ? left : CHUNK;
        esp_codec_dev_write(s_speaker, zero, n * (int)sizeof(zero[0]));
    }
}

static void alert_task(void *arg)
{
    (void)arg;
    uint8_t token;
    for (;;) {
        xQueueReceive(s_queue, &token, portMAX_DELAY);
        if (s_motor_ok) {
            bsp_motor_set_strength(60);
        }
        if (s_audio_ok) {
            /* Rising "ding-dong": A5 then E6, played twice. */
            for (int r = 0; r < 2; r++) {
                play_note(880.0f, 140);
                play_note(1318.5f, 220);
                silence(120);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (s_motor_ok) {
            bsp_motor_set(false);
        }
        xQueueReset(s_queue); /* drop alerts that piled up while playing */
    }
}

void alert_init(void)
{
#if CONFIG_FR_ALERT_VIBRATE
    if (bsp_motor_init() == ESP_OK) {
        bsp_motor_set(false);
        s_motor_ok = true;
    } else {
        ESP_LOGW(TAG, "Vibration off: motor init failed");
    }
#endif
#if CONFIG_FR_ALERT_SOUND
    init_speaker();
#endif
    if (!s_audio_ok && !s_motor_ok) {
        ESP_LOGI(TAG, "Overhead alert: screen only");
        return;
    }
    s_queue = xQueueCreate(2, sizeof(uint8_t));
    xTaskCreate(alert_task, "alert", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Overhead alert ready: sound=%s vibration=%s", s_audio_ok ? "on" : "off",
             s_motor_ok ? "on" : "off");
}

void alert_overhead(void)
{
    if (s_queue) {
        uint8_t token = 1;
        xQueueSend(s_queue, &token, 0);
    }
}
