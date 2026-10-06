#include "Display.h"

#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"

// ESP32-C3-kort med inbyggd 0.42" OLED: SSD1306-styrkrets, 72x40 synliga pixlar.
static U8G2_SSD1306_72X40_ER_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, CFG_OLED_SCL, CFG_OLED_SDA);

static void centered(const char* text, int baseline) {
  const int x = (oled.getDisplayWidth() - oled.getStrWidth(text)) / 2;
  oled.drawStr(x < 0 ? 0 : x, baseline, text);
}

void displayBegin() {
  Wire.begin(CFG_OLED_SDA, CFG_OLED_SCL);
  oled.begin();
  oled.setContrast(200);
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);  // 10 tecken × 6 px = 60 px
  centered("Guest wifi", 16);
  oled.setFont(u8g2_font_5x8_tr);   // 13 tecken × 5 px = 65 px
  centered("pwd generator", 31);
  oled.sendBuffer();
}
