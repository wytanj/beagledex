/*
 * touch-smoke — prove the touch panel, and learn its coordinate mapping.
 *
 * Navigation is the prerequisite for a multi-app shell: one button cannot carry
 * a watch UI, and GPIO0 is already push-to-talk. So this sketch exists to answer
 * three questions before any shell gets designed around the answers:
 *
 *   1. Which CST816-family part is actually fitted (register 0xA7).
 *   2. Do reported coordinates land where the finger did, or are the axes
 *      swapped/inverted relative to the panel's orientation?
 *   3. Which gestures does this part report natively, so the shell can use them
 *      instead of reimplementing swipe detection from raw coordinates.
 *
 * WHY THERE IS NO RESET OR INTERRUPT PIN HERE: neither is recorded in
 * pin_config.h, and neither is needed. The chip answered an I2C scan at 0x15
 * while running, so it is powered and addressable as-is; polling its registers
 * costs nothing at watch frame rates and removes two unknown pins from the
 * problem. The panel reset is behind a TCA9554 expander (0x20) and the touch
 * reset is likely the same, which is a bridge to cross only if polling fails.
 *
 * Tap the four labelled corner boxes. Dots trail your finger so tracking is
 * visible, the readout shows raw coordinates, and a long press clears the
 * screen. If a dot appears diagonally opposite your finger, the mapping needs
 * inverting — which is exactly what this sketch is for finding out cheaply.
 *
 * Build:  node scripts/build.mjs firmware/touch-smoke
 */
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "pin_config.h"

/* ── CST816-family touch ──────────────────────────────────────────────────────
 * Register map is common across CST716/816D/816S/816T/820/826/836 — verified
 * against the CST816S datasheet and Espressif's esp_lcd_touch_cst816s.
 */
static constexpr uint8_t TOUCH_ADDR   = 0x15;
static constexpr uint8_t REG_GESTURE  = 0x01;
static constexpr uint8_t REG_FINGERS  = 0x02;
static constexpr uint8_t REG_XPOS_H   = 0x03;   // 0x03..0x06 = XH,XL,YH,YL
static constexpr uint8_t REG_CHIP_ID  = 0xA7;
static constexpr uint8_t REG_SLEEP    = 0xFE;   // DisAutoSleep: 1 = stay awake

static bool readRegs(uint8_t reg, uint8_t *out, size_t n) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;    // keep the bus, then read
  if (Wire.requestFrom((int)TOUCH_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

/* Only 0xB4 is a confident identification (CST816S). The rest of the family
 * shares this register map, so log the raw byte rather than assert a name we
 * cannot stand behind — guessing a part number is what cost us the panel driver. */
static const char *chipName(uint8_t id) {
  switch (id) {
    case 0xB4: return "CST816S";
    case 0xB5: return "CST816-family";
    case 0xB6: return "CST816-family";
    case 0xB7: return "CST816-family (CST820?)";
    default:   return "unrecognised";
  }
}

static const char *gestureName(uint8_t g) {
  switch (g) {
    case 0x00: return "none";
    case 0x01: return "swipe up";
    case 0x02: return "swipe down";
    case 0x03: return "swipe left";
    case 0x04: return "swipe right";
    case 0x05: return "tap";
    case 0x0B: return "double tap";
    case 0x0C: return "long press";
    default:   return "?";
  }
}

/* ── display ──────────────────────────────────────────────────────────────── */

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);

static constexpr uint16_t C_BG   = RGB565(0, 0, 0);
static constexpr uint16_t C_DIM  = RGB565(130, 138, 150);
static constexpr uint16_t C_MARK = RGB565(16, 148, 152);
static constexpr uint16_t C_DOT  = RGB565(64, 208, 120);
static constexpr int16_t  BOXW   = 54;
static constexpr int16_t  READY  = 200;   // y of the coordinate readout

static uint8_t chipId = 0;

static void drawChrome() {
  gfx->fillScreen(C_BG);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 96);
  gfx->print("tap the corners");
  gfx->setTextColor(C_DIM);
  gfx->setCursor(12, 122);
  gfx->printf("0x%02X %s", chipId, chipName(chipId));
  gfx->setCursor(12, 148);
  gfx->print("long press clears");

  /* Corner targets, inset so a fingertip can actually land inside one. Labels
   * name the corner, so a report of "I pressed TL and the dot lit BR" is an
   * unambiguous statement about the mapping. */
  struct { int16_t x, y; const char *label; } corners[] = {
    {0,                 0,                  "TL"},
    {LCD_WIDTH - BOXW,  0,                  "TR"},
    {0,                 LCD_HEIGHT - BOXW,  "BL"},
    {LCD_WIDTH - BOXW,  LCD_HEIGHT - BOXW,  "BR"},
  };
  for (auto &c : corners) {
    gfx->drawRect(c.x, c.y, BOXW, BOXW, C_MARK);
    gfx->setTextColor(C_MARK);
    gfx->setTextSize(2);
    gfx->setCursor(c.x + 14, c.y + 20);
    gfx->print(c.label);
  }
}

static void drawReadout(uint16_t x, uint16_t y, uint8_t fingers, uint8_t gesture) {
  gfx->fillRect(12, READY, LCD_WIDTH - 24, 52, C_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, READY);
  gfx->printf("x %3u  y %3u  n %u", x, y, fingers);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(12, READY + 26);
  gfx->printf("gesture %s", gestureName(gesture));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n\ntouch-smoke — CST816 @ 0x%02X, CO5300 %dx%d\n",
                TOUCH_ADDR, LCD_WIDTH, LCD_HEIGHT);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);

  uint8_t id = 0;
  if (readRegs(REG_CHIP_ID, &id, 1)) {
    chipId = id;
    Serial.printf("[info/touch] chip id 0x%02X — %s\n", id, chipName(id));
  } else {
    Serial.println("[error/touch] no answer at 0x15 — check SDA=15 SCL=14");
  }

  // Keep it awake, or it stops reporting after a few idle seconds and looks broken.
  if (!writeReg(REG_SLEEP, 0x01)) Serial.println("[warn/touch] could not disable auto-sleep");

  if (!gfx->begin()) {
    Serial.println("[error/panel] gfx->begin() failed");
    return;
  }
  gfx->setBrightness(255);
  drawChrome();
  Serial.println("[info/touch] ready — tap the corners, long press clears");
}

void loop() {
  uint8_t r[6] = {0};
  if (!readRegs(REG_GESTURE, r, sizeof r)) { delay(50); return; }

  const uint8_t  gesture = r[0];
  const uint8_t  fingers = r[1];
  const uint16_t x = ((uint16_t)(r[2] & 0x0F) << 8) | r[3];   // 12-bit, high nibble only
  const uint16_t y = ((uint16_t)(r[4] & 0x0F) << 8) | r[5];

  static uint32_t lastLog = 0;
  if (fingers > 0) {
    gfx->fillCircle(x, y, 5, C_DOT);
    drawReadout(x, y, fingers, gesture);

    // One line every 150 ms: enough to see the shape of a swipe in the console
    // without burning a file per frame in the log store.
    if (millis() - lastLog > 150) {
      lastLog = millis();
      Serial.printf("[info/touch] x=%u y=%u fingers=%u gesture=0x%02X %s\n",
                    x, y, fingers, gesture, gestureName(gesture));
    }
  }

  if (gesture == 0x0C) {          // long press — start over
    drawChrome();
    Serial.println("[info/touch] cleared");
    delay(400);
  }

  delay(16);                      // ~60 Hz, plenty for a finger
}
