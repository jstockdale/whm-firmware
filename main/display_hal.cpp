/*
 * display_hal.cpp - C bridge over esphome/esp-hub75's Hub75Driver.
 *
 * Mirrors the structure of Waveshare's hub75_bridge.cpp but takes pins and
 * panel geometry from whm_board.h (single source of truth in this tree)
 * instead of Kconfig. Driver-internal compile-time knobs (bit depth, gamma,
 * IRAM placement) still come from sdkconfig.defaults.
 */
#include "display_hal.h"
#include "whm_board.h"

#include "hub75.h"
#include "esp_log.h"

static const char *TAG = "whm_disp";
static Hub75Driver *s_driver = nullptr;

extern "C" bool whm_display_init(uint8_t initial_brightness)
{
    if (s_driver) {
        return true;
    }

    Hub75Config cfg{};
    cfg.panel_width  = WHM_PANEL_W;
    cfg.panel_height = WHM_PANEL_H;
    cfg.scan_wiring  = Hub75ScanWiring::STANDARD_TWO_SCAN;   /* 1/32-scan 64x64 */
    cfg.shift_driver = Hub75ShiftDriver::FM6126A;            /* known-good for
                                                                these Adafruit
                                                                P2.5 panels */
    cfg.layout_rows  = 1;
    cfg.layout_cols  = 1;
    cfg.layout       = Hub75PanelLayout::HORIZONTAL;
    cfg.rotation     = Hub75Rotation::ROTATE_0;

    Hub75Pins pins{};
    pins.r1  = WHM_HUB75_R1;
    pins.g1  = WHM_HUB75_G1;
    pins.b1  = WHM_HUB75_B1;
    pins.r2  = WHM_HUB75_R2;
    pins.g2  = WHM_HUB75_G2;
    pins.b2  = WHM_HUB75_B2;
    pins.a   = WHM_HUB75_A;
    pins.b   = WHM_HUB75_B;
    pins.c   = WHM_HUB75_C;
    pins.d   = WHM_HUB75_D;
    pins.e   = WHM_HUB75_E;
    pins.lat = WHM_HUB75_LAT;
    pins.oe  = WHM_HUB75_OE;
    pins.clk = WHM_HUB75_CLK;
    cfg.pins = pins;

    /* FULL DEPTH (owner: "the panel is the whole point"): 32 MHz
       is the one clock that lets the driver's auto-picker reach
       lsbMsbTransitionBit=0 at the 120 Hz target - every bit
       plane correctly binary-weighted, the once-shown-LSB dim
       flicker class gone at the ROOT (150Hz@20M,T=1 -> 240@32M
       -> ~120@T=0). FM6126A typ tops 30 MHz; 32 is the proven
       short-leads setting. If a panel objects (shimmer/ghost
       columns), the one-line fallback is HZ_27M - which keeps
       T=1, so report it instead if possible. */
    cfg.output_clock_speed = Hub75ClockSpeed::HZ_32M;
    cfg.min_refresh_rate   = 120;
    cfg.latch_blanking     = 1;
    cfg.double_buffer      = true;   /* tear-free flips, core of the sync design */
    cfg.clk_phase_inverted = false;  /* board-validated (Arduino demo: clkphase=false) */
    cfg.brightness         = initial_brightness;

    s_driver = new Hub75Driver(cfg);
    if (!s_driver || !s_driver->begin()) {
        ESP_LOGE(TAG, "Hub75Driver begin() failed");
        delete s_driver;
        s_driver = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "HUB75 up: %ux%u, FM6126A init, 20MHz, double-buffered, depth=%d",
             s_driver->get_width(), s_driver->get_height(), CONFIG_HUB75_BIT_DEPTH);
    return true;
}

extern "C" void whm_display_clear(void)
{
    if (s_driver) s_driver->clear();
}

extern "C" void whm_display_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_driver) s_driver->fill(0, 0, s_driver->get_width(), s_driver->get_height(), r, g, b);
}

extern "C" void whm_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      uint8_t r, uint8_t g, uint8_t b)
{
    if (s_driver) s_driver->fill(x, y, w, h, r, g, b);
}

extern "C" void whm_display_set_pixel(uint16_t x, uint16_t y,
                                      uint8_t r, uint8_t g, uint8_t b)
{
    if (s_driver) s_driver->set_pixel(x, y, r, g, b);
}

extern "C" void whm_display_blit_rgb888(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                        const uint8_t *buf)
{
    if (s_driver) {
        s_driver->draw_pixels(x, y, w, h, buf, Hub75PixelFormat::RGB888,
                              Hub75ColorOrder::RGB, false);
    }
}

extern "C" void whm_display_flip(void)
{
    if (s_driver) s_driver->flip_buffer();
}

extern "C" void whm_display_set_brightness(uint8_t b)
{
    if (s_driver) s_driver->set_brightness(b);
}

extern "C" uint16_t whm_display_width(void)
{
    return s_driver ? s_driver->get_width() : 0;
}

extern "C" uint16_t whm_display_height(void)
{
    return s_driver ? s_driver->get_height() : 0;
}

extern "C" bool whm_display_running(void)
{
    return s_driver && s_driver->is_running();
}
