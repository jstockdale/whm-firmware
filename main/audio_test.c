/*
 * audio_test.c - ES8311 (speaker) + ES7210 (dual mic) bring-up.
 *
 * Init sequence mirrors Waveshare's bsp_audio_* functions: one duplex I2S
 * std-mode channel pair feeds both codecs; esp_codec_dev wraps ES8311
 * (DAC, pa_pin=GPIO11) and ES7210 (ADC). Then:
 *   1. 1kHz sine burst to the speaker (audible pass/fail)
 *   2. 1s mic capture -> per-channel RMS + peak (clap to verify)
 */
#include "audio_test.h"
#include "settings.h"
#include "whm_board.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "sensors_test.h"   /* whm_i2c_bus_handle() */

static const char *TAG = "whm_audio";

#define WHM_AUDIO_RATE   48000
#define WHM_AUDIO_CH     2
#define WHM_AUDIO_BITS   16

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static const audio_codec_data_if_t *s_i2s_data = NULL;
static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;

static esp_err_t i2s_up(void)
{
    if (s_tx && s_rx) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(WHM_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s chan");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WHM_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = WHM_I2S_MCLK,
            .bclk = WHM_I2S_BCLK,
            .ws   = WHM_I2S_WS,
            .dout = WHM_I2S_DOUT,
            .din  = WHM_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx en");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx en");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = WHM_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_i2s_data = audio_codec_new_i2s_data(&i2s_cfg);
    return s_i2s_data ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t codecs_up(void)
{
    if (s_spk && s_mic) return ESP_OK;
    ESP_RETURN_ON_ERROR(i2s_up(), TAG, "i2s");

    i2c_master_bus_handle_t bus = whm_i2c_bus_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    /* ---- speaker: ES8311 ---- */
    audio_codec_i2c_cfg_t spk_i2c = {
        .port = WHM_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = spk_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = WHM_PA_ENABLE,
        .pa_reverted = WHM_PA_REVERTED,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) return ESP_FAIL;
    esp_codec_dev_cfg_t spk_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if = s_i2s_data,
    };
    s_spk = esp_codec_dev_new(&spk_cfg);

    /* ---- mics: ES7210 ---- */
    audio_codec_i2c_cfg_t mic_i2c = {
        .port = WHM_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    es7210_codec_cfg_t es7210_cfg = { .ctrl_if = mic_ctrl };
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
    if (!es7210_if) return ESP_FAIL;
    esp_codec_dev_cfg_t mic_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_if,
        .data_if = s_i2s_data,
    };
    s_mic = esp_codec_dev_new(&mic_cfg);

    return (s_spk && s_mic) ? ESP_OK : ESP_FAIL;
}

static bool s_mic_streaming = false;

bool whm_audio_mic_active(void) { return s_mic_streaming; }

esp_err_t whm_audio_mic_start(void)
{
    ESP_RETURN_ON_ERROR(codecs_up(), TAG, "codec init");
    if (s_mic_streaming) return ESP_OK;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = WHM_AUDIO_RATE,
        .channel = WHM_AUDIO_CH,
        .bits_per_sample = WHM_AUDIO_BITS,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_in_gain(s_mic, 30.0);
    s_mic_streaming = true;
    return ESP_OK;
}

/* read ~10ms and return per-channel RMS (raw 16-bit units) */
esp_err_t whm_audio_mic_read_rms(float *l, float *r)
{
    if (!s_mic_streaming) return ESP_ERR_INVALID_STATE;
    enum { FR = WHM_AUDIO_RATE / 100 };
    static int16_t buf[FR * WHM_AUDIO_CH];
    if (esp_codec_dev_read(s_mic, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    double s2l = 0, s2r = 0;
    for (int i = 0; i < FR; i++) {
        int32_t a = buf[2 * i], b = buf[2 * i + 1];
        s2l += (double)a * a;
        s2r += (double)b * b;
    }
    *l = sqrtf((float)(s2l / FR));
    *r = sqrtf((float)(s2r / FR));
    return ESP_OK;
}

void whm_audio_mic_stop(void)
{
    if (s_mic_streaming) {
        esp_codec_dev_close(s_mic);
        s_mic_streaming = false;
    }
}

static bool s_spk_busy = false;

/* Recursive sine resonator: y[n] = c*y[n-1] - y[n-2], c = 2cos(w).
 * Two multiplies per sample per voice, exact sine, zero libm in the
 * render loop - synthesis cost becomes irrelevant to scheduling. */
static inline float osc_step(float c, float *y1, float *y2)
{
    float y = c * (*y1) - (*y2);
    *y2 = *y1;
    *y1 = y;
    return y;
}

static inline void osc_init(float hz, float *c, float *y1, float *y2)
{
    float w = 2.0f * (float)M_PI * hz / WHM_AUDIO_RATE;
    *c = 2.0f * cosf(w);
    *y1 = sinf(-w);
    *y2 = sinf(-2.0f * w);
}
static uint8_t s_sys_vol = 0;              /* 0 = not yet loaded */

uint8_t whm_audio_sys_vol(void)
{
    if (!s_sys_vol) {
        uint32_t v = 85;                    /* louder default */
        whm_settings_get_u32("vol", &v);
        s_sys_vol = (uint8_t)(v < 1 ? 1 : v > 100 ? 100 : v);
    }
    return s_sys_vol;
}

esp_err_t whm_audio_set_sys_vol(uint8_t v)
{
    if (v < 1 || v > 100) return ESP_ERR_INVALID_ARG;
    s_sys_vol = v;
    return whm_settings_set_u32("vol", v);
}

bool whm_audio_spk_busy(void) { return s_spk_busy; }

esp_err_t whm_audio_spk_open(uint32_t rate, uint8_t ch, uint8_t vol_pct)
{
    ESP_RETURN_ON_ERROR(codecs_up(), TAG, "codec init");
    if (s_spk_busy) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate, .channel = ch, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_out_vol(s_spk, vol_pct);
    s_spk_busy = true;
    return ESP_OK;
}

esp_err_t whm_audio_spk_write(const void *data, size_t len)
{
    if (!s_spk_busy) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_write(s_spk, (void *)data, len) == ESP_CODEC_DEV_OK
               ? ESP_OK : ESP_FAIL;
}

void whm_audio_spk_setvol(uint8_t vol_pct)
{
    if (s_spk_busy) esp_codec_dev_set_out_vol(s_spk, vol_pct);
}

void whm_audio_spk_close(void)
{
    if (s_spk_busy) {
        esp_codec_dev_close(s_spk);
        s_spk_busy = false;
    }
}

static void beep_task(void *arg)
{
    (void)arg;
    static volatile bool busy = false;
    if (busy) { vTaskDelete(NULL); return; }
    busy = true;
    do {
        if (whm_audio_spk_open(WHM_AUDIO_RATE, WHM_AUDIO_CH, whm_audio_sys_vol()) != ESP_OK) {
            ESP_LOGW(TAG, "beep skipped (speaker busy - music playing?)");
            break;
        }
        enum { FR = WHM_AUDIO_RATE / 8 };            /* 125ms chunks */
        int16_t *buf = malloc((size_t)FR * WHM_AUDIO_CH * sizeof(int16_t));
        if (buf) {
            for (int rep = 0; rep < 3; rep++) {
                float ph = 0;
                const float dphi = 2.0f * (float)M_PI * 880.0f / WHM_AUDIO_RATE;
                const int BA = WHM_AUDIO_RATE * 3 / 1000;
                for (int i = 0; i < FR; i++) {
                    float e = 1.0f;
                    if (i < BA) e = (float)i / (float)BA;
                    if (FR - i < BA) e = (float)(FR - i) / (float)BA;
                    int16_t v = (int16_t)(sinf(ph) * e * 11000.0f);
                    ph += dphi;
                    buf[2 * i] = v;
                    buf[2 * i + 1] = v;
                }
                whm_audio_spk_write(buf, FR * WHM_AUDIO_CH * sizeof(int16_t));
                memset(buf, 0, (size_t)FR * WHM_AUDIO_CH * sizeof(int16_t) / 2);
                whm_audio_spk_write(buf, FR * WHM_AUDIO_CH * sizeof(int16_t) / 2);
            }
            free(buf);
        }
        whm_audio_spk_close();
    } while (0);
    busy = false;
    vTaskDelete(NULL);
}

/* Boot chime: G4-C5-E5-G5 ascent into a C6+G5 chord, sine voices with a
 * 6ms attack and exponential decay - cheerful, ~1.1s, then gone. Yields
 * silently if the speaker is busy. */
static void chime_task(void *arg)
{
    (void)arg;
    do {
        unsigned cv = (unsigned)whm_audio_sys_vol();
        whm_audio_spk_close();   /* clear any boot-left half-state so
                                    this open performs a fresh enable */
        int ch_ok = 0, ch_bad = 0;
        esp_err_t ch_first = ESP_OK;
        printf("chime: vol=%u\n", cv);   /* names a silent-mute in one line */
        if (whm_audio_spk_open(WHM_AUDIO_RATE, WHM_AUDIO_CH,
                               (int)cv) != ESP_OK) {
            ESP_LOGW(TAG, "chime skipped - spk open failed (internal %u KB free)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
                                / 1024));
            break;
        }
        /* Every note carries a 6ms attack and an 8ms RELEASE RAMP TO
         * TRUE ZERO - handoffs happen at silence, so phase resets can't
         * click. Envelope decay is an incremental per-sample multiplier;
         * oscillators are recursive resonators. */
        static const struct { float f1, f2; int ms; float tau_ms; } notes[] = {
            { 392.00f,    0.0f, 130,  95.0f },    /* G4 */
            { 523.25f,    0.0f, 130,  95.0f },    /* C5 */
            { 659.25f,    0.0f, 130,  95.0f },    /* E5 */
            { 783.99f,    0.0f, 150, 110.0f },    /* G5 */
            { 1046.50f, 783.99f, 620, 300.0f },   /* C6+G5, rings out */
        };
        enum { CHUNK = 1024 };
        size_t bsz = (size_t)CHUNK * WHM_AUDIO_CH * sizeof(int16_t);
        int16_t *buf = heap_caps_malloc(bsz, MALLOC_CAP_SPIRAM);
        if (!buf) buf = malloc(bsz);
        if (!buf) {
            /* THE FAMINE-ERA SILENT DEATH: this alloc failed at 14KB
               free and the old bare if(buf) skipped the whole song
               without a word. Never again. */
            printf("chime: buf alloc FAILED (internal %u KB free)\n",
                   (unsigned)(heap_caps_get_free_size(
                       MALLOC_CAP_INTERNAL) / 1024));
        }
        if (buf) {
            printf("chime: playing (vol=%u)\n", cv);
            /* 20ms silent preroll: let the DAC unmute settle off-mic */
            memset(buf, 0, (size_t)CHUNK * WHM_AUDIO_CH * sizeof(int16_t));
            whm_audio_spk_write(buf, (size_t)(WHM_AUDIO_RATE / 50) *
                                         WHM_AUDIO_CH * sizeof(int16_t));
            const int A = WHM_AUDIO_RATE * 6 / 1000;    /* attack  */
            const int R = WHM_AUDIO_RATE * 8 / 1000;    /* release */
            for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
                int total = WHM_AUDIO_RATE * notes[n].ms / 1000;
                float kdec = expf(-1000.0f /
                                  (notes[n].tau_ms * WHM_AUDIO_RATE));
                float dec = 1.0f;
                float c1, o1a, o1b, c2 = 0, o2a = 0, o2b = 0;
                osc_init(notes[n].f1, &c1, &o1a, &o1b);
                if (notes[n].f2 > 0.0f) {
                    osc_init(notes[n].f2, &c2, &o2a, &o2b);
                }
                int done = 0;
                while (done < total) {
                    int fr = total - done > CHUNK ? CHUNK : total - done;
                    for (int i2 = 0; i2 < fr; i2++) {
                        int nn = done + i2;
                        float env = dec;
                        if (nn < A) env *= (float)nn / (float)A;
                        int rem = total - nn;
                        if (rem < R) env *= (float)rem / (float)R;
                        float smp = osc_step(c1, &o1a, &o1b);
                        if (notes[n].f2 > 0.0f) {
                            smp = 0.55f * smp +
                                  0.42f * osc_step(c2, &o2a, &o2b);
                        }
                        int16_t v = (int16_t)(smp * env * 8200.0f);
                        buf[2 * i2] = v;
                        buf[2 * i2 + 1] = v;
                        dec *= kdec;
                    }
                    esp_err_t wrc = whm_audio_spk_write(buf,
                        (size_t)fr * WHM_AUDIO_CH * sizeof(int16_t));
                    if (wrc == ESP_OK) {
                        ch_ok++;
                    } else {
                        ch_bad++;
                        if (ch_first == ESP_OK) ch_first = wrc;
                    }
                    done += fr;
                }
            }
            /* drain: 40ms of true zeros before close */
            memset(buf, 0, (size_t)CHUNK * WHM_AUDIO_CH * sizeof(int16_t));
            whm_audio_spk_write(buf, (size_t)(WHM_AUDIO_RATE * 4 / 100) *
                                         WHM_AUDIO_CH * sizeof(int16_t));
            free(buf);
        }
        if (buf) {
            if (ch_bad == 0) {
                printf("chime: done (%d chunks)\n", ch_ok);
            } else {
                printf("chime: done (%d ok, %d BAD - first err %s; "
                       "out channel wedged?)\n",
                       ch_ok, ch_bad, esp_err_to_name(ch_first));
            }
        }
        whm_audio_spk_close();
    } while (0);
    vTaskDelete(NULL);
}

void whm_audio_chime_async(void)
{
    xTaskCreatePinnedToCore(chime_task, "whm_chime", 2816, NULL, 10,
                            NULL, 0);
}

typedef struct { float hz; int ms; } tone_req_t;

static void tone_task(void *arg)
{
    tone_req_t rq = *(tone_req_t *)arg;
    free(arg);
    do {
        if (whm_audio_spk_open(WHM_AUDIO_RATE, WHM_AUDIO_CH,
                               whm_audio_sys_vol()) != ESP_OK) {
            printf("tone skipped (speaker busy)\n");
            break;
        }
        enum { CHUNK = 1024 };
        int16_t *buf = malloc((size_t)CHUNK * WHM_AUDIO_CH * sizeof(int16_t));
        if (buf) {
            memset(buf, 0, (size_t)CHUNK * WHM_AUDIO_CH * sizeof(int16_t));
            whm_audio_spk_write(buf, (size_t)(WHM_AUDIO_RATE / 50) *
                                         WHM_AUDIO_CH * sizeof(int16_t));
            int total = WHM_AUDIO_RATE * rq.ms / 1000;
            const int A = WHM_AUDIO_RATE * 5 / 1000;
            const int R = WHM_AUDIO_RATE * 8 / 1000;
            float kdec = expf(-1000.0f /
                              (rq.ms * 0.9f * WHM_AUDIO_RATE));
            float dec = 1.0f, c1, o1a, o1b;
            osc_init(rq.hz, &c1, &o1a, &o1b);
            int done = 0;
            while (done < total) {
                int fr = total - done > CHUNK ? CHUNK : total - done;
                for (int i2 = 0; i2 < fr; i2++) {
                    int nn = done + i2;
                    float env = dec;
                    if (nn < A) env *= (float)nn / (float)A;
                    int rem = total - nn;
                    if (rem < R) env *= (float)rem / (float)R;
                    int16_t v = (int16_t)(osc_step(c1, &o1a, &o1b) *
                                          env * 8800.0f);
                    buf[2 * i2] = v;
                    buf[2 * i2 + 1] = v;
                    dec *= kdec;
                }
                whm_audio_spk_write(buf, (size_t)fr * WHM_AUDIO_CH *
                                             sizeof(int16_t));
                done += fr;
            }
            memset(buf, 0, (size_t)CHUNK * WHM_AUDIO_CH * sizeof(int16_t));
            whm_audio_spk_write(buf, (size_t)(WHM_AUDIO_RATE * 3 / 100) *
                                         WHM_AUDIO_CH * sizeof(int16_t));
            free(buf);
        }
        whm_audio_spk_close();
    } while (0);
    vTaskDelete(NULL);
}

esp_err_t whm_audio_tone_async(float hz, int ms)
{
    if (hz < 40 || hz > 12000 || ms < 20 || ms > 5000) {
        return ESP_ERR_INVALID_ARG;
    }
    tone_req_t *rq = malloc(sizeof(*rq));
    if (!rq) return ESP_ERR_NO_MEM;
    rq->hz = hz;
    rq->ms = ms;
    xTaskCreatePinnedToCore(tone_task, "whm_tone", 2816, rq, 10,
                            NULL, 0);
    return ESP_OK;
}

void whm_audio_beep_async(void)
{
    xTaskCreatePinnedToCore(beep_task, "whm_beep", 2816, NULL, 10, NULL, 0);
}

esp_err_t whm_audio_test(void)
{
    if (whm_audio_spk_busy()) {
        printf("audio test unavailable while music is playing - "
               "'mp3 stop' first\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mic_streaming) {
        printf("audio test unavailable while the VU screen is live - "
               "switch screens first\n");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(codecs_up(), TAG, "codec init");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = WHM_AUDIO_RATE,
        .channel = WHM_AUDIO_CH,
        .bits_per_sample = WHM_AUDIO_BITS,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return ESP_FAIL;
    }
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed");
        esp_codec_dev_close(s_spk);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, 65);
    esp_codec_dev_set_in_gain(s_mic, 30.0);

    /* ---- 1kHz tone, 1.2s ---- */
    ESP_LOGI(TAG, "speaker: 1kHz tone burst (listen for it)");
    const int frames = WHM_AUDIO_RATE / 10;              /* 100ms chunks */
    int16_t *chunk = malloc((size_t)frames * WHM_AUDIO_CH * sizeof(int16_t));
    if (!chunk) return ESP_ERR_NO_MEM;
    float phase = 0.0f;
    const float dphi = 2.0f * (float)M_PI * 1000.0f / WHM_AUDIO_RATE;
    for (int rep = 0; rep < 12; rep++) {
        for (int i = 0; i < frames; i++) {
            int16_t v = (int16_t)(sinf(phase) * 9000.0f);   /* ~-11 dBFS */
            phase += dphi;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            chunk[i * 2 + 0] = v;
            chunk[i * 2 + 1] = v;
        }
        esp_codec_dev_write(s_spk, chunk, frames * WHM_AUDIO_CH * sizeof(int16_t));
    }

    /* ---- mic capture, 1s, RMS/peak per channel ---- */
    ESP_LOGI(TAG, "mic: capturing 1s (make some noise)");
    double sum2[2] = {0, 0};
    int32_t peak[2] = {0, 0};
    size_t total_frames = 0;
    for (int rep = 0; rep < 10; rep++) {
        if (esp_codec_dev_read(s_mic, chunk,
                               frames * WHM_AUDIO_CH * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed at rep %d", rep);
            break;
        }
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                int32_t v = chunk[i * 2 + c];
                sum2[c] += (double)v * v;
                int32_t a = v < 0 ? -v : v;
                if (a > peak[c]) peak[c] = a;
            }
        }
        total_frames += frames;
    }
    free(chunk);

    if (total_frames) {
        float rms_l = sqrtf((float)(sum2[0] / total_frames));
        float rms_r = sqrtf((float)(sum2[1] / total_frames));
        ESP_LOGI(TAG, "mic L: rms=%.0f peak=%ld | mic R: rms=%.0f peak=%ld "
                      "(silence ~<100 rms; speech/claps should spike)",
                 rms_l, (long)peak[0], rms_r, (long)peak[1]);
    }

    esp_codec_dev_close(s_spk);
    esp_codec_dev_close(s_mic);
    ESP_LOGI(TAG, "audio path validated (codecs stay initialized)");
    return ESP_OK;
}
