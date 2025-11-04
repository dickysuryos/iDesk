#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// ==== PINS (adjust) ====
#define SDA_PIN     8
#define SCL_PIN     9
#define RESET_PIN   U8X8_PIN_NONE   // e.g. 10 if your OLED RES is wired to GPIO10

// ==== I2C address (7-bit from scanner) ====
#define I2C_ADDR_7BIT  0x3C
#define I2C_ADDR_8BIT  (I2C_ADDR_7BIT << 1)  // U8g2 uses 8-bit in setI2CAddress()

// Try hardware I2C (fast) and software I2C (fallback)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C  u8g2_hw_ssd1306(U8G2_R0, RESET_PIN, /* SCL */ SCL_PIN, /* SDA */ SDA_PIN);
U8G2_SH1106_128X64_NONAME_F_HW_I2C   u8g2_hw_sh1106(U8G2_R0, RESET_PIN, /* SCL */ SCL_PIN, /* SDA */ SDA_PIN);
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2_hw_ssd1306_128x32(U8G2_R0, RESET_PIN, SCL_PIN, SDA_PIN);

U8G2_SSD1306_128X64_NONAME_F_SW_I2C  u8g2_sw_ssd1306(U8G2_R0, /* SCL */ SCL_PIN, /* SDA */ SDA_PIN, RESET_PIN);

void drawTest(U8G2 &d, const char* label) {
  d.clearBuffer();
  d.setContrast(255);                 // max contrast to be obvious
  d.setFont(u8g2_font_6x10_tr);
  d.drawStr(0, 12, "Hello IC Display!");
  d.drawStr(0, 24, label);
  d.drawFrame(0, 32, 64, 16);
  d.sendBuffer();
}

bool tryDriver(const char* name, U8G2 &d, bool setAddr=true) {
  Serial.print("Trying "); Serial.println(name);
  if (setAddr) d.setI2CAddress(I2C_ADDR_8BIT);  // ensure 0x3C is used
  // Lower I2C speed for reliability during bring-up
  Wire.setClock(100000);                         // 100 kHz
  d.begin();
  drawTest(d, name);
  delay(300);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nU8g2 bring-up on 0x3C");

  // Start I2C explicitly and slow to avoid marginal wiring issues
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  // OPTIONAL: toggle reset pin if you wired it
  if (RESET_PIN != U8X8_PIN_NONE) {
    pinMode(RESET_PIN, OUTPUT);
    digitalWrite(RESET_PIN, LOW);  delay(10);
    digitalWrite(RESET_PIN, HIGH); delay(10);
  }

  // Try likely combos in order:
//   tryDriver("SSD1306 128x64 HW_I2C", u8g2_hw_ssd1306);
//   tryDriver("SH1106 128x64 HW_I2C",  u8g2_hw_sh1106);
//   tryDriver("SSD1306 128x32 HW_I2C", u8g2_hw_ssd1306_128x32);

  // Fallback to software I2C (bit-bang) if HW timing is unhappy
  tryDriver("SSD1306 128x64 SW_I2C", u8g2_sw_ssd1306);

  Serial.println("If none showed, check notes below.");
}

void loop() {}
