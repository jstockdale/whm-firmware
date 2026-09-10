#pragma once
/* From docs/monitor-hw-pro.md - LilyGO T-Display-S3 Pro (MVSRLora)
 * ST7796 SPI+DC, 49-row GRAM gap in landscape, PWM backlight,
 * CST226SE touch, LR1121 radio (pins banked for the gateway). */
#define MON_BOARD_NAME   "s3pro"
#define MON_BL_PWM       1      /* brightness via LEDC on BL pin */
#define MON_COL_COMPACT  1
#define MON_HAS_LORA     1
#define MP_TFT_CS    39
#define MP_TFT_DC     9
#define MP_TFT_SCK   18
#define MP_TFT_MOSI  17
#define MP_TFT_MISO   8
#define MP_TFT_RST   47
#define MP_TFT_BL    48
#define MP_MADCTL_L  0x68       /* MX|MV|BGR - landscape 480x222 */
#define MP_GAP_X      0
#define MP_GAP_Y     49         /* the 49-col GRAM offset, moved to
                                   the row axis by MV - swap gaps if
                                   a band appears (verify on glass) */
#define MP_TOUCH_SDA  5
#define MP_TOUCH_SCL  6
#define MP_TOUCH_RST 13
#define MP_TOUCH_IRQ 21
#define MP_TOUCH_ADDR 0x5A      /* CST226SE */
#define MP_BTN_BOOT   0
#define MP_LORA_CS    7         /* the camera sacrifice */
#define MP_LORA_BUSY 46
#define MP_LORA_IRQ  40
#define MP_LORA_RST  10
#define LCD_W       480
#define LCD_H       222

#define MP_UI_MS  33
#define MP_UI_FPS 30
