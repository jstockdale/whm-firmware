#pragma once
/* From docs/monitor-hw.md - LilyGO T-Display-S3 AMOLED (TOUCH) */
#define MON_BOARD_NAME   "amoled191"
#define MON_BL_DCS       1      /* brightness via DCS 0x51 */
#define MON_COL_COMPACT  0
#define MON_HAS_LORA     0
#define MP_PMIC_EN   38   /* HIGH before init or the panel stays dark */
#define MP_QSPI_CS    6
#define MP_QSPI_SCK  47
#define MP_QSPI_D0   18
#define MP_QSPI_D1    7
#define MP_QSPI_D2   48
#define MP_QSPI_D3    5
#define MP_LCD_RST   17
#define MP_TOUCH_SDA  3
#define MP_TOUCH_SCL  2
#define MP_TOUCH_IRQ 21
#define MP_BTN_BOOT   0
#define LCD_W       536
#define LCD_H       240

#define MP_UI_MS  16
#define MP_UI_FPS 60
