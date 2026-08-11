/*
 * panel-smoke — prove the AMOLED lights up. Nothing else.
 *
 * Phase 4 (`ui.display`) starts here, and deliberately in isolation: this
 * sketch touches no audio, no PSRAM and no WiFi, so if pixels do not appear
 * there is exactly one subsystem to blame. Same reasoning as phase 00 for the
 * audio path.
 *
 * THE DRIVER IS CO5300, NOT SH8601. Two hardware revisions of this board
 * exist and they use different panel controllers. The factory image dumped
 * off this very unit — ~/esp32-backups/28848590ba0c-full-16mb.bin — contains
 * the string "co5300" 33 times and "sh8601" not once, so this is the v2
 * panel regardless of what the older notes in pin_config.h said. Arduino_GFX
 * ships both drivers and the wrong one gives you a plausible, silent, black
 * screen, so this is worth being sure about rather than sorry.
 *
 * RST is GFX_NOT_DEFINED because the panel reset is not an ESP32 GPIO on this
 * board — there is a TCA9554 I2C expander handling it (the factory image
 * references that too). Nothing here needs it: the vendor's own Hello World
 * brings this panel up over QSPI alone, with no expander and no AXP2101 call.
 *
 * col_offset1 = 16 is not decoration. This panel's visible area starts 16
 * columns in; without the offset everything is shifted sideways. That is the
 * vendor's own value for this display.
 *
 * The frame counter in loop() is the point of the whole sketch. This board has
 * already produced one frozen vendor framebuffer that survived every reset, so
 * a *static* image on this panel proves nothing at all. A number that ticks
 * proves the ESP32 is actually rendering, right now.
 *
 * Build:  node scripts/build.mjs firmware/panel-smoke
 *         (not `npm run fw:build`, which has translator-p01 baked in)
 * Flash:  npm run flash -- --port COM3 --version 0.2.0 --features ui.display \
 *           --bin firmware/panel-smoke/build/panel-smoke.ino.bin
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pin_config.h"

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus,
    GFX_NOT_DEFINED,          /* RST — not an ESP32 pin on this board */
    0,                        /* rotation */
    LCD_WIDTH, LCD_HEIGHT,    /* 368 x 448; the driver defaults to 480x480 */
    16, 0, 0, 0);             /* col_offset1 = 16 — see above */

static bool     ready  = false;
static uint32_t frames = 0;

void setup() {
  Serial.begin(115200);
  delay(300);                 // let USB CDC enumerate before the first print
  Serial.printf("\n\npanel-smoke — CO5300 %dx%d over QSPI\n", LCD_WIDTH, LCD_HEIGHT);
  Serial.printf("[info/panel] pins  cs=%d sclk=%d d0=%d d1=%d d2=%d d3=%d\n",
                LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

  if (!gfx->begin()) {
    // Deliberately keep running rather than halting: the serial line staying
    // alive is what tells you the sketch got this far at all.
    Serial.println("[error/panel] gfx->begin() failed — QSPI bus or wrong panel driver");
    return;
  }
  ready = true;
  Serial.println("[info/panel] gfx->begin() ok");

  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(255);    // an AMOLED at brightness 0 is indistinguishable
                              // from a dead one, so never leave this to chance
  Serial.println("[info/panel] cleared, brightness 255");

  /* Colour bars first. If these show but in the wrong colours, the panel is
   * alive and the byte order is wrong — a completely different bug from a dead
   * screen, and one worth being able to tell apart at a glance. */
  const uint16_t bars[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE};
  const int16_t  bw     = LCD_WIDTH / 4;
  for (int i = 0; i < 4; i++) gfx->fillRect(i * bw, 0, bw, 64, bars[i]);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(10, 96);
  gfx->println("translator");

  gfx->setTextSize(2);
  gfx->setCursor(10, 150);
  gfx->printf("CO5300 %dx%d", LCD_WIDTH, LCD_HEIGHT);

  Serial.println("[info/panel] drew bars + text — the panel should not be black any more");
}

void loop() {
  if (!ready) { delay(1000); return; }

  /* Repaint only the counter's own strip. A full-screen repaint every tick
   * would be pure waste, and this board's PSRAM is quad rather than octal, so
   * display bandwidth is worth not squandering once audio shares the bus. */
  frames++;
  gfx->fillRect(10, 210, LCD_WIDTH - 20, 44, RGB565_BLACK);
  gfx->setTextColor(RGB565_GREEN);
  gfx->setTextSize(3);
  gfx->setCursor(10, 210);
  gfx->printf("frame %lu", (unsigned long)frames);

  if (frames % 10 == 0) Serial.printf("[info/panel] frame %lu\n", (unsigned long)frames);
  delay(500);
}
