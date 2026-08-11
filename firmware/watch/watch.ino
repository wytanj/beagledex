/*
 * watch — the app shell. Phase 04.
 *
 * The device's primary firmware. translator-p01 stays as the isolated audio
 * proof it always was; this is what the pocket watch actually runs.
 *
 * THE CENTRAL DESIGN DECISION
 *
 * Push-to-talk into an LLM is not a translator feature — it is *the* interaction
 * primitive, and most apps will be a variation on it. So the shell owns it:
 *
 *   · the shell owns the buttons, the capture, the token stream and the chrome
 *   · an app declares only which prompt it wants and how to draw what comes back
 *
 * Adding the tenth app costs about twenty lines. Translation is deliberately not
 * special-cased anywhere: it is an Ask with a different prompt.
 *
 * POWER (the AMOLED is the battery, so this matters most)
 *
 *   AWAKE → DIM → OFF, on an idle timer, and any touch or button press returns
 *   to AWAKE. OFF calls displayOff() rather than only zeroing brightness,
 *   because on an AMOLED the panel controller keeps self-refreshing otherwise —
 *   which is exactly how this board sat there burning current with a frozen
 *   vendor frame on it for a day.
 *
 *   While OFF the shell stops ticking apps and stops drawing entirely, and polls
 *   input at a slower cadence. What it deliberately does NOT do is light-sleep
 *   the SoC: that drops the USB-Serial/JTAG link, and the serial line is
 *   currently the only transport this device has. Revisit after phase 3.
 *
 * BUTTONS — there are exactly two, so each has to earn its keep
 *
 *   BOOT (GPIO0), the one confirmed readable:
 *     tap        lock, or wake if the screen is off   (the iPhone side button)
 *     double tap next app — a touch-free path through the whole UI, so a failed
 *                touch panel cannot strand you
 *     hold       voice: captures while held, from locked too
 *
 *   PWR: wired to the AXP2101's PWRKEY, not to a GPIO, so it cannot be read with
 *   digitalRead. Waveshare document it as customisable but publish neither the
 *   register nor the method. pmicIrqPeek() below reads the PMIC's interrupt
 *   status bytes read-only and logs them when they change, so pressing PWR will
 *   reveal which bit it owns. Using it then needs one write to clear the latch —
 *   deliberately not done until the bit is confirmed. No guessed PMIC writes.
 *
 * QUICK SETTINGS
 *
 *   Swipe down from the top strip. Brightness, sleep timeout, lock now. Vertical
 *   swipes that do not start at the top still belong to the app, so a future app
 *   can scroll without fighting the shell.
 *
 * THE REPAINT RULE, WHICH IS NOT NEGOTIABLE
 *
 *   Nothing paints while audio is capturing. Quad PSRAM plus a PSRAM capture
 *   buffer means display traffic during capture causes crackle. Enforced
 *   centrally via audioBusy so no app can get it wrong.
 *
 * Board: Waveshare ESP32-S3-Touch-AMOLED-1.8, v2 parts throughout (CO5300 panel,
 * CST820 touch), all confirmed off this unit. See pin_config.h.
 *
 * Build:  npm run watch:build
 */
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <esp_mac.h>
#include <esp_partition.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "pin_config.h"
#include "shell_types.h"   // Gesture + App — must arrive via a header, see that file

extern "C" {
#include "es8311.h"
}

static const char *FW_VERSION = "0.7.0";

/* ── audio config ─────────────────────────────────────────────────────────── */

static constexpr uint32_t SAMPLE_RATE = AUDIO_SAMPLE_RATE;
static constexpr uint32_t MAX_SECONDS = 30;
static constexpr size_t   CHANNELS    = 2;   // ES8311 is mono but the frame has 2 slots
static constexpr size_t   MAX_SAMPLES = SAMPLE_RATE * CHANNELS * MAX_SECONDS;
static constexpr size_t   CHUNK       = 512;

I2SClass        i2s;
es8311_handle_t codec  = nullptr;
static int16_t *buffer = nullptr;
static size_t   recorded = 0;
static int32_t  lastPeak = 0;
static uint32_t lastRms  = 0;
static bool     haveAudio = false;

static char deviceId[13] = {0};

/* ── logging ──────────────────────────────────────────────────────────────── */

static void logLine(const char *level, const char *tag, const char *msg) {
  Serial.printf("[%s/%s] %s\n", level, tag, msg);
}

#define LOGI(tag, ...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); logLine("info",  tag, _b); } while (0)
#define LOGW(tag, ...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); logLine("warn",  tag, _b); } while (0)
#define LOGE(tag, ...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); logLine("error", tag, _b); } while (0)

/* ── display ──────────────────────────────────────────────────────────────── */

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);

static bool haveDisplay = false;

static constexpr uint16_t C_BG     = RGB565(0, 0, 0);
static constexpr uint16_t C_CARD   = RGB565(22, 24, 28);
static constexpr uint16_t C_DIM    = RGB565(130, 138, 150);
static constexpr uint16_t C_FAINT  = RGB565(70, 76, 86);
static constexpr uint16_t C_ACCENT = RGB565(16, 148, 152);
static constexpr uint16_t C_OK     = RGB565(64, 208, 120);
static constexpr uint16_t C_HOT    = RGB565(232, 72, 72);
static constexpr uint16_t C_COOL   = RGB565(96, 168, 248);
static constexpr uint16_t C_WARN   = RGB565(240, 176, 64);

/* The shell owns STATUS/TITLE/HINT; apps get BODY and nothing else. That is what
 * keeps the chrome consistent as apps come and go. */
static constexpr int16_t PAD      = 12;
static constexpr int16_t STATUS_H = 40;
static constexpr int16_t TITLE_Y  = 46;
static constexpr int16_t BODY_Y   = 88;
static constexpr int16_t BODY_H   = 316;
static constexpr int16_t HINT_Y   = 418;
static constexpr int16_t BODY_W   = LCD_WIDTH - 2 * PAD;

static void clearBody() { gfx->fillRect(0, BODY_Y, LCD_WIDTH, BODY_H, C_BG); }

/* ── power ────────────────────────────────────────────────────────────────────
 * Settings live here rather than in an app, because sleep behaviour is a
 * property of the device and every app inherits it. These are exactly the values
 * the web UI will configure once there is a transport to carry them.
 */
enum PowerState : uint8_t { PWR_AWAKE, PWR_DIM, PWR_OFF };

static PowerState powerState   = PWR_AWAKE;
static uint32_t   lastActivity = 0;
static uint32_t   dimAfterMs   = 15000;
static uint32_t   offAfterMs   = 45000;   // 0 disables sleeping entirely
static uint8_t    brightFull   = 255;
static uint8_t    brightDim    = 40;

/* ── cache: the SD card that isn't fitted yet ─────────────────────────────────
 * Everything that will want the card — recorded WAV, the offline phrase cache,
 * token transcripts — goes through this seam, so the day the card arrives this
 * gains a body and no caller changes.
 *
 * THE SD PINS ARE NOT KNOWN. They are absent from pin_config.h, absent from
 * Waveshare's documentation page, and the vendor's own SD example includes a
 * pin_config.h their repository does not ship. Do not guess them: a wrong SDMMC
 * pin assignment can drive a pin that is doing something else. Leads, in order
 * of expected reliability: the board schematic PDF on files.waveshare.com,
 * xiaozhi-esp32's board config for this board, then the vendor 14_LVGL_SD_Test.
 */
static bool cacheReady() { return false; }

static bool cachePut(const char *key, const uint8_t *data, size_t len) {
  (void)data;
  if (!cacheReady()) { LOGW("cache", "no card: dropped %s (%u bytes)", key, (unsigned)len); return false; }
  return false;
}

static size_t cacheGet(const char *key, uint8_t *out, size_t cap) {
  (void)out; (void)cap;
  if (!cacheReady()) { LOGW("cache", "no card: miss %s", key); return 0; }
  return 0;
}

/* ── CST820 touch, polled ─────────────────────────────────────────────────────
 * The gesture engine reports 0x00 for everything when polled, so swipes are
 * derived here from coordinate deltas. Verified against this unit.
 */
static constexpr uint8_t T_ADDR = 0x15;

static bool touchRead(uint8_t *r6) {
  Wire.beginTransmission(T_ADDR);
  Wire.write((uint8_t)0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)T_ADDR, 6) != 6) return false;
  for (int i = 0; i < 6; i++) r6[i] = Wire.read();
  return true;
}

static bool     tDown = false;
static uint16_t tX0, tY0, tXl, tYl;
static uint32_t tT0 = 0;
static uint16_t tapX = 0, tapY = 0;      // where the last tap landed
static uint16_t swipeStartY = 0;         // where the last swipe began

static Gesture settleTouch() {
  tDown = false;
  const int32_t  dx = (int32_t)tXl - (int32_t)tX0;
  const int32_t  dy = (int32_t)tYl - (int32_t)tY0;
  const uint32_t dt = millis() - tT0;
  swipeStartY = tY0;
  if (dt > 900) return G_NONE;                          // a drag, not a flick
  if (abs(dx) > 60 && abs(dx) > abs(dy)) return dx < 0 ? G_SWIPE_L : G_SWIPE_R;
  if (abs(dy) > 60 && abs(dy) > abs(dx)) return dy < 0 ? G_SWIPE_U : G_SWIPE_D;
  if (abs(dx) < 24 && abs(dy) < 24) { tapX = tXl; tapY = tYl; return G_TAP; }
  return G_NONE;
}

static Gesture pollGesture() {
  uint8_t r[6];
  if (!touchRead(r)) {
    // A failed read while a finger is down would otherwise strand tDown forever
    // and kill navigation until reboot. Treat it as a release.
    return tDown ? settleTouch() : G_NONE;
  }
  const bool     down = r[1] > 0;
  const uint16_t x = ((uint16_t)(r[2] & 0x0F) << 8) | r[3];
  const uint16_t y = ((uint16_t)(r[4] & 0x0F) << 8) | r[5];

  if (down) {
    if (!tDown) { tDown = true; tX0 = x; tY0 = y; tT0 = millis(); }
    tXl = x; tYl = y;
    return G_NONE;
  }
  return tDown ? settleTouch() : G_NONE;
}

/* Any finger contact at all — the wake check, which must not wait for a
 * complete, classified gesture. */
static bool touchPresent() {
  uint8_t r[6];
  return touchRead(r) && r[1] > 0;
}

/* ── PCF85063 RTC ─────────────────────────────────────────────────────────────
 * The oscillator-stop flag is honoured rather than ignored: an unset clock says
 * so instead of confidently showing 00:00. Set with
 *   npm run push time
 */
static constexpr uint8_t RTC_ADDR = 0x51;

struct RtcTime { uint16_t year; uint8_t mon, day, hour, min, sec; bool valid; };
static RtcTime rtc = {0, 0, 0, 0, 0, 0, false};

static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static bool rtcReadNow() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_ADDR, 7) != 7) return false;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();

  rtc.valid = (b[0] & 0x80) == 0;      // bit7 set = oscillator stopped, time is junk
  rtc.sec   = bcd2dec(b[0] & 0x7F);
  rtc.min   = bcd2dec(b[1] & 0x7F);
  rtc.hour  = bcd2dec(b[2] & 0x3F);
  rtc.day   = bcd2dec(b[3] & 0x3F);
  rtc.mon   = bcd2dec(b[5] & 0x1F);
  rtc.year  = (uint16_t)(2000 + bcd2dec(b[6]));
  return true;
}

static bool rtcSet(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x04);
  Wire.write(dec2bcd(s));              // writing seconds clears the stop flag
  Wire.write(dec2bcd(mi));
  Wire.write(dec2bcd(h));
  Wire.write(dec2bcd(d));
  Wire.write((uint8_t)0);              // weekday: unused here
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd((uint8_t)(y % 100)));
  return Wire.endTransmission() == 0;
}

/* ── AXP2101 ──────────────────────────────────────────────────────────────────
 * Battery percentage at 0xA4 is XPowersLib's fuel gauge, PROVISIONAL: it has not
 * had the two-independent-sources treatment the pins got, so it is sanity-bounded
 * and renders as "--" when implausible.
 *
 * Everything here READS. Nothing writes to the PMIC. This chip controls the
 * system rails, so a guessed write is not a bug you debug — it is a bug that
 * turns the board off, or worse.
 */
static constexpr uint8_t PMIC_ADDR = 0x34;

static bool pmicRead(uint8_t reg, uint8_t *out, size_t n) {
  Wire.beginTransmission(PMIC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)PMIC_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}

/* DO NOT TRUST THIS YET. 0xA4 is XPowersLib's fuel-gauge percentage, but measured
 * on this unit it read 100% within minutes of the cell being reported at 1% — a
 * charge rate no lithium cell achieves. So it is either not the percentage, or it
 * saturates whenever VBUS is present and says nothing about the battery.
 *
 * The consequence is worse than a cosmetic wrong number: it means percentage
 * cannot be used to measure power work. Until this is resolved, the trustworthy
 * metric is wall-clock hours to shutdown on battery, which is slow but honest.
 * Fixing it properly means reading the battery voltage ADC and converting, which
 * needs a register this project has not yet verified against two sources.
 */
static int batteryPct() {
  uint8_t v = 0;
  if (!pmicRead(0xA4, &v, 1)) return -1;
  return (v <= 100) ? (int)v : -1;
}

/* PWR button discovery. Reads the three interrupt-status bytes and logs them
 * whenever they change; press PWR and whichever bit moves is the one it owns.
 * Read-only on purpose — turning this into a usable button needs a write to
 * clear the latch, and that write is not going in until the bit is confirmed. */
static uint8_t pmicIrq[3] = {0, 0, 0};

static void pmicIrqPeek() {
  uint8_t v[3];
  if (!pmicRead(0x48, v, 3)) return;
  if (v[0] != pmicIrq[0] || v[1] != pmicIrq[1] || v[2] != pmicIrq[2]) {
    LOGI("pmic", "irq status %02X %02X %02X (was %02X %02X %02X) — PWR candidate",
         v[0], v[1], v[2], pmicIrq[0], pmicIrq[1], pmicIrq[2]);
    pmicIrq[0] = v[0]; pmicIrq[1] = v[1]; pmicIrq[2] = v[2];
  }
}

/* ── token sink ───────────────────────────────────────────────────────────────
 * Where the LLM's reply accumulates. A fixed buffer on purpose: a watch has no
 * business growing a heap allocation per token, and dropping the oldest text is
 * the right failure mode for something you glance at.
 */
static constexpr size_t TOK_CAP = 512;
static char   tokBuf[TOK_CAP] = {0};
static size_t tokLen = 0;
static bool   tokDirty = false;

static void tokClear() { tokLen = 0; tokBuf[0] = 0; tokDirty = true; }

static void tokAppend(const char *s) {
  while (*s && tokLen < TOK_CAP - 1) tokBuf[tokLen++] = *s++;
  tokBuf[tokLen] = 0;
  if (tokLen >= TOK_CAP - 1) {        // keep the tail, drop the head
    const size_t keep = TOK_CAP / 2;
    memmove(tokBuf, tokBuf + tokLen - keep, keep);
    tokLen = keep;
    tokBuf[tokLen] = 0;
  }
  tokDirty = true;
}

/* Word-wrapped text at size 2 — 12 px per character, 344 px of body. */
static void drawWrapped(const char *s, int16_t x, int16_t y, int16_t maxY, uint16_t colour) {
  gfx->setTextSize(2);
  gfx->setTextColor(colour);
  const int cols = BODY_W / 12;
  char line[64];
  int16_t cy = y;
  while (*s && cy < maxY) {
    const char *sp = nullptr;
    int n = 0;
    while (s[n] && n < cols) { if (s[n] == ' ') sp = s + n; n++; }
    int take = (s[n] && sp && n >= cols) ? (int)(sp - s) : n;
    if (take <= 0) take = n;
    memcpy(line, s, take);
    line[take] = 0;
    gfx->setCursor(x, cy);
    gfx->print(line);
    cy += 20;
    s += take;
    while (*s == ' ') s++;
  }
}

/* ── app state ────────────────────────────────────────────────────────────── */

static int  appIndex  = 0;
static bool audioBusy = false;        // set across capture; blocks every repaint
static bool qsOpen    = false;        // quick settings panel

/* ── the watch face ───────────────────────────────────────────────────────────
 * The face is DATA, not code. A background image plus a descriptor saying where
 * the time sits, in what size and colour, both living in the storage partition —
 * so a new face is an upload, not a firmware build. That distinction is the whole
 * reason the face is not "just another app": if it were, every change of
 * wallpaper would need a reflash.
 *
 * WHERE IT LIVES. storage at 0x710000, in slots of 0x52000: descriptor at +0,
 * pixels at +0x1000. Two properties come free. The face survives every app
 * reflash, because we only ever write 0x110000.. — and uploading is an esptool
 * partition write, so 322 KB takes about 4 seconds hash-verified rather than 30
 * over the text protocol. scripts/face.py builds and writes the blob.
 *
 * esp_partition_mmap gives a pointer straight into flash, so the photo is blitted
 * with no decode, no copy and no allocation. There is deliberately no JPEG
 * decoder here: the host does the image work, which is the same division of
 * labour as the rest of this device.
 *
 * PHOTO IS EXPENSIVE. AMOLED current scales with lit pixels, and this thing gets
 * eight hours. So the photo shows on wake and then gives way to a dark face with
 * just the time — bright pixels only while you are actually looking at them. Tap
 * to bring the photo back.
 */
static constexpr uint32_t FACE_MAGIC   = 0x31434657;   // 'WFC1', little-endian
static constexpr uint32_t FACE_STRIDE   = 0x52000;
static constexpr uint32_t FACE_PIX_OFF  = 0x1000;

struct FaceDesc {
  uint16_t w, h;
  uint16_t timeX, timeY;
  uint8_t  timeSize;
  uint16_t timeColour;
  bool     showDate;
  uint16_t holdMs;
};

/* Defaults are the no-photo face, so a device with an empty slot still tells the
 * time properly instead of showing an error. */
static FaceDesc face = { LCD_WIDTH, LCD_HEIGHT, 40, 180, 7, RGB565(255, 255, 255), true, 3000 };

static const uint16_t             *facePix = nullptr;
static esp_partition_mmap_handle_t faceMap = 0;

static void faceLoad(int slot) {
  if (faceMap) { esp_partition_munmap(faceMap); faceMap = 0; }
  facePix = nullptr;

  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
  if (!p) { LOGW("face", "no storage partition — digital face"); return; }

  const size_t base = (size_t)slot * FACE_STRIDE;
  uint8_t h[32];
  if (esp_partition_read(p, base, h, sizeof h) != ESP_OK) {
    LOGW("face", "slot %d unreadable — digital face", slot); return;
  }

  uint32_t magic, pixBytes;
  uint16_t w, ht, tx, ty, colour, hold;
  memcpy(&magic, h + 0x00, 4);
  if (magic != FACE_MAGIC) { LOGI("face", "slot %d empty — digital face", slot); return; }

  memcpy(&w, h + 0x04, 2);
  memcpy(&ht, h + 0x06, 2);
  const uint8_t fmt = h[0x08], flags = h[0x09];
  memcpy(&pixBytes, h + 0x0C, 4);
  memcpy(&tx, h + 0x14, 2);
  memcpy(&ty, h + 0x16, 2);
  memcpy(&colour, h + 0x1A, 2);
  memcpy(&hold, h + 0x1C, 2);

  // Validate before blitting. A bad header would otherwise paint 322 KB of
  // arbitrary flash onto the panel, which looks like a hardware fault.
  if (w != LCD_WIDTH || ht != LCD_HEIGHT || pixBytes != (uint32_t)w * ht * 2 || fmt != 0) {
    LOGW("face", "slot %d implausible: %ux%u fmt %u %u bytes — digital face",
         slot, w, ht, fmt, (unsigned)pixBytes);
    return;
  }

  const void *ptr = nullptr;
  if (esp_partition_mmap(p, base + FACE_PIX_OFF, pixBytes,
                         ESP_PARTITION_MMAP_DATA, &ptr, &faceMap) != ESP_OK) {
    LOGW("face", "slot %d mmap failed — digital face", slot); return;
  }

  facePix        = (const uint16_t *)ptr;
  face.w         = w;
  face.h         = ht;
  face.timeX     = tx;
  face.timeY     = ty;
  face.timeSize  = h[0x18] ? h[0x18] : 7;
  face.timeColour = colour;
  face.showDate  = (flags & 0x01) != 0;
  face.holdMs    = hold ? hold : 3000;
  LOGI("face", "slot %d mapped %ux%u, time (%u,%u) size %u, hold %u ms",
       slot, w, ht, tx, ty, face.timeSize, face.holdMs);
}

enum FaceMode : uint8_t { FACE_PHOTO, FACE_DARK };
static FaceMode faceMode    = FACE_DARK;
static uint32_t faceSince   = 0;
static uint8_t  faceLastMin = 255;

static void faceDrawTime() {
  const int16_t ch = 8 * face.timeSize;
  // In dark mode the background is flat, so clear just the text block. In photo
  // mode the whole photo is re-blitted by the caller instead — cheaper to reason
  // about than a sub-rectangle restore, and it happens at most once a minute
  // during the few seconds the photo is even up.
  if (faceMode == FACE_DARK)
    gfx->fillRect(face.timeX, face.timeY, 6 * face.timeSize * 5 + 6, ch + 28, C_BG);

  gfx->setTextSize(face.timeSize);
  gfx->setTextColor(face.timeColour);
  gfx->setCursor(face.timeX, face.timeY);
  if (rtc.valid) gfx->printf("%02u:%02u", rtc.hour, rtc.min);
  else           gfx->print("--:--");

  if (face.showDate) {
    gfx->setTextSize(2);
    gfx->setTextColor(rtc.valid ? C_DIM : C_WARN);
    gfx->setCursor(face.timeX + 2, face.timeY + ch + 8);
    if (rtc.valid) gfx->printf("%04u-%02u-%02u", rtc.year, rtc.mon, rtc.day);
    else           gfx->print("npm run push time");
  }
}

static void faceEnter() {
  if (facePix) {
    faceMode  = FACE_PHOTO;
    faceSince = millis();
    gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)facePix, face.w, face.h);
  } else {
    faceMode = FACE_DARK;
    gfx->fillScreen(C_BG);
  }
  faceDrawTime();
  faceLastMin = rtc.min;
}

static void faceTick(uint32_t now) {
  if (faceMode == FACE_PHOTO && facePix && now - faceSince > face.holdMs) {
    faceMode = FACE_DARK;
    gfx->fillScreen(C_BG);              // drop the expensive pixels
    faceDrawTime();
    faceLastMin = rtc.min;
    return;
  }
  if (rtc.min != faceLastMin) {
    faceLastMin = rtc.min;
    if (faceMode == FACE_PHOTO) gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)facePix, face.w, face.h);
    faceDrawTime();
  }
}

static void faceGesture(Gesture g) {
  if (g == G_TAP && facePix) faceEnter();   // show me the dog again
}

/* ── app: ask (the PTT → LLM primitive) ───────────────────────────────────────
 * Every future app of this kind is a copy of this with a different prompt. The
 * app does not capture audio and does not own a button — the shell hands it a
 * finished capture and a token stream, and it decides only how to draw them.
 */
static void askPaintTokens() {
  gfx->fillRect(PAD, BODY_Y + 96, BODY_W, BODY_H - 100, C_BG);
  if (tokLen) drawWrapped(tokBuf, PAD, BODY_Y + 96, BODY_Y + BODY_H, RGB565_WHITE);
  else        drawWrapped("no reply yet. hold BOOT, speak, release. tokens arrive over serial until phase 3 lands.",
                          PAD, BODY_Y + 96, BODY_Y + BODY_H, C_FAINT);
  tokDirty = false;
}

static void askStatus(const char *s, uint16_t colour) {
  gfx->fillRect(PAD, BODY_Y, BODY_W, 40, C_BG);
  gfx->setTextSize(3); gfx->setTextColor(colour);
  gfx->setCursor(PAD, BODY_Y + 4);
  gfx->print(s);
}

static void askMeter(int32_t peak) {
  gfx->fillRect(PAD, BODY_Y + 52, BODY_W, 24, C_BG);
  gfx->drawRect(PAD, BODY_Y + 52, BODY_W, 24, C_CARD);
  gfx->fillRect(PAD, BODY_Y + 80, BODY_W, 12, C_BG);
  if (peak > 0) {
    const float db = 20.0f * log10f((float)peak / 32767.0f);
    float f = (db + 60.0f) / 60.0f;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    const int16_t w = (int16_t)(f * (BODY_W - 2));
    gfx->fillRect(PAD + 1, BODY_Y + 53, w, 22, f > 0.85f ? C_HOT : C_OK);
    gfx->setTextSize(1); gfx->setTextColor(C_DIM);
    gfx->setCursor(PAD + 2, BODY_Y + 80);
    gfx->printf("peak %ld  %.0f dBFS", (long)peak, db);
  } else {
    gfx->setTextSize(1); gfx->setTextColor(C_WARN);
    gfx->setCursor(PAD + 2, BODY_Y + 80);
    gfx->print("no mic signal");
  }
}

static void askEnter() {
  clearBody();
  askStatus(haveAudio ? "READY" : "NO AUDIO", haveAudio ? C_OK : C_HOT);
  askMeter(lastPeak);
  askPaintTokens();
}
static void askTick(uint32_t) { if (tokDirty) askPaintTokens(); }
static void askGesture(Gesture g) { if (g == G_TAP) { tokClear(); askPaintTokens(); } }
static void askCapture(float secs) {
  askStatus("SENT", C_COOL);
  askMeter(lastPeak);
  gfx->setTextSize(1); gfx->setTextColor(C_DIM);
  gfx->setCursor(PAD + 200, BODY_Y + 80);
  gfx->printf("%.1f s captured", secs);
  // Where a transport will go. Until phase 3, the host answers over serial.
  LOGI("ask", "capture ready: %.2f s, %u samples, peak %ld",
       secs, (unsigned)recorded, (long)lastPeak);
}

/* ── app: system ──────────────────────────────────────────────────────────── */

static const char *powerName() {
  switch (powerState) {
    case PWR_AWAKE: return "awake";
    case PWR_DIM:   return "dim";
    default:        return "off";
  }
}

static void sysPaint() {
  clearBody();
  gfx->setTextSize(2); gfx->setTextColor(C_DIM);
  int16_t y = BODY_Y + 2;
  const int bat = batteryPct();
  gfx->setCursor(PAD, y); gfx->printf("fw      %s", FW_VERSION);                        y += 24;
  gfx->setCursor(PAD, y); gfx->printf("device  %s", deviceId);                          y += 24;
  gfx->setCursor(PAD, y); gfx->printf("up      %lu s", millis() / 1000);                y += 24;
  gfx->setCursor(PAD, y); gfx->printf("heap    %u KB", ESP.getFreeHeap() / 1024);       y += 24;
  gfx->setCursor(PAD, y); gfx->printf("psram   %u KB", ESP.getFreePsram() / 1024);      y += 24;
  if (bat >= 0) { gfx->setCursor(PAD, y); gfx->printf("battery %d%%", bat); }
  else          { gfx->setCursor(PAD, y); gfx->print("battery --  unverified"); }
  y += 24;
  gfx->setCursor(PAD, y); gfx->printf("power   %s", powerName());                       y += 24;
  gfx->setCursor(PAD, y); gfx->printf("sleep   %lu s", (unsigned long)(offAfterMs / 1000)); y += 24;
  gfx->setCursor(PAD, y); gfx->printf("audio   %s", haveAudio ? "up" : "down");         y += 24;
  gfx->setCursor(PAD, y); gfx->printf("card    %s", cacheReady() ? "ready" : "none");   y += 24;
  gfx->setCursor(PAD, y); gfx->printf("pmic    %02X %02X %02X", pmicIrq[0], pmicIrq[1], pmicIrq[2]);
}
static void sysEnter() { sysPaint(); }
static void sysTick(uint32_t now) {
  static uint32_t last = 0;
  if (now - last > 2000) { last = now; sysPaint(); }
}

/* ── the registry ─────────────────────────────────────────────────────────── */

/* Slot 0 is the face and is full-bleed. The id stays `app.clock` even though it
 * now means considerably more than a clock — same reasoning as audio.loopback:
 * the id is a stable key, the meaning is allowed to move. */
static App APPS[] = {
  { "app.clock",  "Face",   "tap for the photo",         faceEnter,  faceTick,  faceGesture, nullptr,    true  },
  { "app.ask",    "Ask",    "hold BOOT \xB7 tap clears", askEnter,   askTick,   askGesture,  askCapture, false },
  { "app.system", "System", "swipe down for settings",   sysEnter,   sysTick,   nullptr,     nullptr,    false },
};
static constexpr int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

/* ── shell chrome ─────────────────────────────────────────────────────────── */

static void drawStatus() {
  gfx->fillRect(0, 0, LCD_WIDTH, STATUS_H, C_CARD);
  gfx->setTextSize(2); gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(PAD, 12);
  if (rtc.valid) gfx->printf("%02u:%02u", rtc.hour, rtc.min);
  else           gfx->print("--:--");

  const int bat = batteryPct();
  gfx->setTextColor(C_DIM);
  gfx->setCursor(LCD_WIDTH - PAD - 96, 12);
  if (bat >= 0) gfx->printf("%3d%%", bat);
  else          gfx->print("  --");
  gfx->setTextColor(haveAudio ? C_OK : C_HOT);
  gfx->setCursor(LCD_WIDTH - PAD - 30, 12);
  gfx->print(haveAudio ? "\xF9" : "x");
}

static void drawTitle() {
  gfx->fillRect(0, TITLE_Y, LCD_WIDTH, 36, C_BG);
  gfx->setTextSize(3); gfx->setTextColor(C_ACCENT);
  gfx->setCursor(PAD, TITLE_Y);
  gfx->print(qsOpen ? "Settings" : APPS[appIndex].title);

  if (!qsOpen) {
    const int16_t dx = LCD_WIDTH - PAD - APP_COUNT * 16;
    for (int i = 0; i < APP_COUNT; i++)
      gfx->fillCircle(dx + i * 16, TITLE_Y + 14, 4, i == appIndex ? C_ACCENT : C_FAINT);
  }
}

static void drawHint() {
  gfx->fillRect(0, HINT_Y, LCD_WIDTH, 24, C_BG);
  gfx->setTextSize(2); gfx->setTextColor(C_FAINT);
  gfx->setCursor(PAD, HINT_Y);
  gfx->print(qsOpen ? "swipe up to close" : APPS[appIndex].hint);
}

static void enterApp(int i) {
  qsOpen = false;
  appIndex = (i + APP_COUNT) % APP_COUNT;
  if (!APPS[appIndex].fullscreen) {
    // Chrome is redrawn on every entry because a full-bleed face wipes it.
    drawStatus();
    drawTitle();
    drawHint();
  }
  if (APPS[appIndex].onEnter) APPS[appIndex].onEnter();
  LOGI("shell", "app %s", APPS[appIndex].title);
}

/* ── power control ────────────────────────────────────────────────────────── */

static void noteActivity() { lastActivity = millis(); }

static void wakeScreen() {
  if (!haveDisplay) return;
  const bool wasOff = powerState == PWR_OFF;
  powerState = PWR_AWAKE;
  gfx->setBrightness(brightFull);
  if (wasOff) {
    gfx->displayOn();
    // Repaint everything: nothing was drawn while the panel was off, and
    // trusting a controller to have kept a framebuffer is how this board ended
    // up displaying a stale vendor frame for a day.
    gfx->fillScreen(C_BG);
    // A watch wakes to its face, not to wherever you last left it. Apps are
    // somewhere you go; the face is where the device lives.
    enterApp(0);
    LOGI("pwr", "awake");
  }
  noteActivity();
}

static void powerTick(uint32_t now) {
  if (!haveDisplay || offAfterMs == 0) return;
  const uint32_t idle = now - lastActivity;

  if (powerState == PWR_AWAKE && idle > dimAfterMs) {
    powerState = PWR_DIM;
    gfx->setBrightness(brightDim);
    LOGI("pwr", "dim after %lu ms idle", (unsigned long)idle);
  } else if (powerState != PWR_OFF && idle > offAfterMs) {
    powerState = PWR_OFF;
    gfx->setBrightness(0);
    // Brightness alone is not enough: the panel controller keeps self-refreshing
    // and the AMOLED keeps drawing current. displayOff() is the part that saves
    // the battery.
    gfx->displayOff();
    LOGI("pwr", "screen off after %lu ms idle", (unsigned long)idle);
  }
}

static void lockNow() {
  if (!haveDisplay || powerState == PWR_OFF) return;
  powerState = PWR_OFF;
  gfx->setBrightness(0);
  gfx->displayOff();
  LOGI("pwr", "locked");
}

/* ── quick settings ───────────────────────────────────────────────────────────
 * Swipe down from the status strip. Rows are fixed-height so a tap maps to a row
 * by arithmetic rather than a hit-test table.
 */
static constexpr int16_t QS_ROW_H = 52;
static constexpr int16_t QS_Y0    = BODY_Y + 40;

static void qsPaint() {
  clearBody();
  gfx->setTextSize(2); gfx->setTextColor(C_DIM);
  gfx->setCursor(PAD, BODY_Y + 6);
  gfx->print("tap a row to change");

  const char *labels[4];
  char b0[40], b1[40], b2[40], b3[40];
  snprintf(b0, sizeof b0, "brightness   %3u", brightFull);
  if (offAfterMs) snprintf(b1, sizeof b1, "sleep after  %lu s", (unsigned long)(offAfterMs / 1000));
  else            snprintf(b1, sizeof b1, "sleep after  never");
  snprintf(b2, sizeof b2, "lock now");
  snprintf(b3, sizeof b3, "audio        %s", haveAudio ? "up" : "down");
  labels[0] = b0; labels[1] = b1; labels[2] = b2; labels[3] = b3;

  for (int i = 0; i < 4; i++) {
    const int16_t y = QS_Y0 + i * QS_ROW_H;
    gfx->fillRoundRect(PAD, y, BODY_W, QS_ROW_H - 8, 8, C_CARD);
    gfx->setTextSize(2);
    gfx->setTextColor(i == 3 ? C_FAINT : RGB565_WHITE);   // audio row is read-only
    gfx->setCursor(PAD + 12, y + 14);
    gfx->print(labels[i]);
  }
  gfx->setTextSize(1); gfx->setTextColor(C_FAINT);
  gfx->setCursor(PAD, QS_Y0 + 4 * QS_ROW_H + 6);
  gfx->print("brightness: tap left half to lower, right half to raise");
}

static void qsOpenPanel() {
  qsOpen = true;
  drawTitle();
  drawHint();
  qsPaint();
  LOGI("shell", "quick settings");
}

static void qsHandleTap(uint16_t x, uint16_t y) {
  if (y < QS_Y0) return;
  const int row = (y - QS_Y0) / QS_ROW_H;
  switch (row) {
    case 0: {                                   // brightness, left down / right up
      const int step = 64;
      int v = (int)brightFull + (x < LCD_WIDTH / 2 ? -step : step);
      if (v < 32)  v = 32;                      // never let the user blind themselves
      if (v > 255) v = 255;
      brightFull = (uint8_t)v;
      gfx->setBrightness(brightFull);
      LOGI("pwr", "brightness %u", brightFull);
      break;
    }
    case 1: {                                   // sleep timeout, cycling
      if      (offAfterMs == 30000)  offAfterMs = 60000;
      else if (offAfterMs == 60000)  offAfterMs = 300000;
      else if (offAfterMs == 300000) offAfterMs = 0;
      else                           offAfterMs = 30000;
      if (offAfterMs && dimAfterMs > offAfterMs / 2) dimAfterMs = offAfterMs / 2;
      LOGI("pwr", "sleep after %lu ms, dim after %lu ms",
           (unsigned long)offAfterMs, (unsigned long)dimAfterMs);
      break;
    }
    case 2: lockNow(); return;                  // no repaint: the screen is off
    default: return;                            // audio row is status only
  }
  qsPaint();
}

/* ── the shared PTT service ───────────────────────────────────────────────────
 * The one place that touches the codec. Repaints happen strictly before and
 * after the capture window, never inside it — enforced here so that no app can
 * violate it even by accident.
 */
static void captureWhileHeld() {
  recorded = 0;
  int16_t scratch[CHUNK];
  int guard = 0;
  while (i2s.available() > 0 && guard++ < 64) i2s.readBytes((char *)scratch, sizeof scratch);

  while (digitalRead(BOOT_BUTTON) == LOW && recorded < MAX_SAMPLES) {
    const size_t room = MAX_SAMPLES - recorded;
    const size_t want = (room < CHUNK ? room : CHUNK) * sizeof(int16_t);
    const size_t got  = i2s.readBytes((char *)(buffer + recorded), want);
    recorded += got / sizeof(int16_t);
  }

  uint64_t sq = 0;
  int32_t  pk = 0;
  const size_t frames = recorded / CHANNELS;
  for (size_t i = 0; i < frames; i++) {
    const int32_t v = buffer[i * CHANNELS];
    sq += (uint64_t)(v * v);
    if (abs(v) > pk) pk = abs(v);
  }
  lastPeak = pk;
  lastRms  = frames ? (uint32_t)sqrt((double)sq / frames) : 0;
}

static void doVoiceCapture() {
  if (!haveAudio) { LOGW("ptt", "no audio — capture skipped"); return; }

  // Hold works from locked, like a phone's assistant button. Wake first so the
  // capture is visible, then never touch the panel again until it is done.
  if (powerState != PWR_AWAKE) wakeScreen();
  if (qsOpen) enterApp(appIndex);

  audioBusy = true;                       // no repaints past this line
  if (appIndex == 1) askStatus("LISTENING", C_HOT);
  es8311_voice_mute(codec, true);
  digitalWrite(PA, LOW);

  captureWhileHeld();
  while (digitalRead(BOOT_BUTTON) == LOW) delay(10);

  // Deliberately stays muted with PA gated — see startAudio(). Nothing plays
  // back in the shell yet, and leaving the amp hot between captures was costing
  // battery around the clock.
  audioBusy = false;                      // repaints allowed again

  const float secs = recorded / (float)CHANNELS / SAMPLE_RATE;
  LOGI("ptt", "captured %.2f s, peak %ld, rms %lu",
       secs, (long)lastPeak, (unsigned long)lastRms);
  if (APPS[appIndex].onCapture) APPS[appIndex].onCapture(secs);
  noteActivity();
}

/* ── BOOT button: tap / double tap / hold ─────────────────────────────────────
 * Two buttons total means each needs to carry more than one job. Double tap is
 * app-switching specifically so there is a route through the entire UI that does
 * not depend on the touch panel working.
 */
static constexpr uint32_t BTN_DEBOUNCE_MS = 25;
static constexpr uint32_t BTN_HOLD_MS     = 600;
static constexpr uint32_t BTN_DOUBLE_MS   = 320;

static bool     btnDown = false, btnHoldFired = false, btnTapPending = false;
static uint32_t btnDownAt = 0, btnLastTapAt = 0;

static void onButtonTap() {
  noteActivity();
  if (powerState != PWR_AWAKE) wakeScreen();   // wake
  else                         lockNow();      // lock
}

static void onButtonDoubleTap() {
  noteActivity();
  if (powerState != PWR_AWAKE) wakeScreen();
  enterApp(appIndex + 1);
}

static void buttonService() {
  const bool     down = digitalRead(BOOT_BUTTON) == LOW;
  const uint32_t now  = millis();

  if (down && !btnDown) { btnDown = true; btnDownAt = now; btnHoldFired = false; return; }

  if (down && btnDown && !btnHoldFired && now - btnDownAt >= BTN_HOLD_MS) {
    btnHoldFired  = true;
    btnTapPending = false;                     // a hold is not a tap
    doVoiceCapture();                          // blocks until release
    btnDown = false;
    return;
  }

  if (!down && btnDown) {
    btnDown = false;
    if (btnHoldFired) return;
    if (now - btnDownAt < BTN_DEBOUNCE_MS) return;
    if (btnTapPending && now - btnLastTapAt < BTN_DOUBLE_MS) {
      btnTapPending = false;
      onButtonDoubleTap();
    } else {
      btnTapPending = true;
      btnLastTapAt  = now;
    }
  }

  // A single tap only resolves once the double-tap window has closed.
  if (btnTapPending && now - btnLastTapAt >= BTN_DOUBLE_MS) {
    btnTapPending = false;
    onButtonTap();
  }
}

/* ── host commands over serial ────────────────────────────────────────────────
 * Stands in for the config and transport layers until phase 3. The verbs are
 * already the ones a web UI would want, because that is where they are going.
 */
static void handleCommand(char *line) {
  if (!strncmp(line, ">tok ", 5))      { tokAppend(line + 5); noteActivity(); }
  else if (!strcmp(line, ">clear"))    { tokClear(); }
  else if (!strcmp(line, ">lock"))     { lockNow(); }
  else if (!strcmp(line, ">wake"))     { wakeScreen(); }
  else if (!strncmp(line, ">bright ", 8)) {
    int v = atoi(line + 8);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    brightFull = (uint8_t)v;
    if (powerState == PWR_AWAKE) gfx->setBrightness(brightFull);
    LOGI("pwr", "brightness %u", brightFull);
  }
  else if (!strncmp(line, ">sleep ", 7)) {
    const long s = atol(line + 7);            // seconds; 0 = never
    offAfterMs = (uint32_t)(s > 0 ? s * 1000 : 0);
    if (offAfterMs && dimAfterMs > offAfterMs / 2) dimAfterMs = offAfterMs / 2;
    LOGI("pwr", "sleep after %ld s", s);
  }
  else if (!strncmp(line, ">face ", 6)) {
    faceLoad(atoi(line + 6));
    if (powerState != PWR_OFF) enterApp(0);
  }
  else if (!strncmp(line, ">app ", 5)) {
    for (int i = 0; i < APP_COUNT; i++)
      if (!strcasecmp(line + 5, APPS[i].title)) { wakeScreen(); enterApp(i); return; }
    LOGW("cmd", "no app named %s", line + 5);
  }
  else if (!strncmp(line, ">time ", 6)) {
    int y, mo, d, h, mi, s;
    if (sscanf(line + 6, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
      if (rtcSet((uint16_t)y, mo, d, h, mi, s)) {
        rtcReadNow();
        LOGI("rtc", "set to %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
        if (powerState != PWR_OFF && !qsOpen) {
          if (APPS[appIndex].fullscreen) faceEnter();   // face owns the whole screen
          else                           drawStatus();
        }
      } else LOGE("rtc", "write failed");
    } else LOGW("cmd", "usage: >time 2026-08-11T09:41:00");
  }
  else LOGW("cmd", "unknown: %s", line);
}

static void pollSerial() {
  static char line[TOK_CAP];
  static size_t n = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { line[n] = 0; if (n) handleCommand(line); n = 0; continue; }
    if (n < sizeof(line) - 1) line[n++] = c;
  }
}

/* ── bring-up ─────────────────────────────────────────────────────────────── */

static bool startAudio() {
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);
  codec = es8311_create(0, ES8311_ADDRRES_0);
  if (!codec) { LOGE("codec", "es8311_create failed at 0x%02X", ES8311_ADDRRES_0); return false; }

  const es8311_clock_config_t clk = {
    .mclk_inverted = false, .sclk_inverted = false, .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256, .sample_frequency = SAMPLE_RATE,
  };
  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    LOGE("codec", "es8311_init failed"); return false;
  }
  es8311_sample_frequency_config(codec, SAMPLE_RATE * 256, SAMPLE_RATE);
  es8311_voice_volume_set(codec, AUDIO_VOICE_VOLUME, nullptr);
  es8311_microphone_config(codec, false);
  es8311_microphone_gain_set(codec, (es8311_mic_gain_t)AUDIO_MIC_GAIN);

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    LOGE("i2s", "begin failed"); return false;
  }
  /* Idle with the amplifier GATED and the DAC muted. An enabled class-D amp with
   * no signal still costs several mA, and the shell has no playback path yet, so
   * there is nothing to keep it on for. Muting the DAC does not touch the ADC, so
   * capture is unaffected — it is the same gate the half-duplex capture already
   * uses. When playback returns, it must raise PA around itself and drop it after,
   * not leave it high. */
  es8311_voice_mute(codec, true);
  digitalWrite(PA, LOW);

  LOGI("codec", "ES8311 up @ %u Hz, gain %d — idling muted, PA gated",
       SAMPLE_RATE, AUDIO_MIC_GAIN);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(350);
  Serial.printf("\n\nwatch %s — app shell, %d apps\n", FW_VERSION, APP_COUNT);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(deviceId, sizeof deviceId, "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("device %s\n", deviceId);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);

  haveDisplay = gfx->begin();
  if (haveDisplay) { gfx->setBrightness(brightFull); gfx->fillScreen(C_BG); }
  else LOGE("panel", "gfx->begin() failed — shell has nowhere to draw");

  buffer = (int16_t *)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
  if (!buffer) LOGE("mem", "ps_malloc failed — PTT disabled, PSRAM not enabled?");

  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  haveAudio = buffer && startAudio();

  rtcReadNow();
  LOGI("rtc", "%s", rtc.valid ? "time is set" : "not set — run npm run push time");
  pmicIrqPeek();
  faceLoad(0);          // before the first paint, so the face can use its photo

  enterApp(0);
  noteActivity();
  LOGI("shell", "ready — tap locks, double tap changes app, hold talks, swipe down for settings");
  LOGI("pwr", "dim at %lu ms, off at %lu ms",
       (unsigned long)dimAfterMs, (unsigned long)offAfterMs);
}

void loop() {
  pollSerial();
  buttonService();

  if (audioBusy) { delay(5); return; }

  const uint32_t now = millis();

  /* Battery telemetry, awake or asleep — the asleep case is the interesting one.
   * One line a minute gives the console a real discharge curve, so the next power
   * change gets measured against this baseline instead of argued about. `npm run
   * log` persists it and /devices/:id already plots a timeline. */
  static uint32_t lastBatt = 0;
  if (lastBatt == 0 || now - lastBatt > 60000) {
    lastBatt = now;
    LOGI("batt", "%d%% state=%s up=%lu s", batteryPct(), powerName(),
         (unsigned long)(now / 1000));
  }

  /* Asleep: no drawing, no app ticks, and a slower input poll. The only jobs are
   * noticing a finger and noticing the button, which buttonService() above has
   * already done. */
  if (powerState == PWR_OFF) {
    if (touchPresent()) wakeScreen();
    delay(120);
    return;
  }

  if (!haveDisplay) { delay(20); return; }

  const Gesture g = pollGesture();
  if (g != G_NONE) {
    noteActivity();
    if (powerState == PWR_DIM) wakeScreen();   // first gesture only un-dims

    if (qsOpen) {
      if (g == G_SWIPE_U)      enterApp(appIndex);      // close
      else if (g == G_TAP)     qsHandleTap(tapX, tapY);
    } else if (g == G_SWIPE_D && swipeStartY < 90) {
      qsOpenPanel();                                    // pulled down from the top
    } else if (g == G_SWIPE_L) {
      enterApp(appIndex + 1);
    } else if (g == G_SWIPE_R) {
      enterApp(appIndex - 1);
    } else if (APPS[appIndex].onGesture) {
      APPS[appIndex].onGesture(g);
    }
  }

  static uint32_t lastTick = 0, lastRtc = 0, lastStatus = 0, lastPmic = 0;
  if (now - lastRtc > 1000)     { lastRtc = now; rtcReadNow(); }
  if (now - lastPmic > 500)     { lastPmic = now; pmicIrqPeek(); }
  if (now - lastStatus > 10000) {
    lastStatus = now;
    if (!qsOpen && !APPS[appIndex].fullscreen) drawStatus();
  }
  if (now - lastTick > 100 && !qsOpen) {
    lastTick = now;
    if (APPS[appIndex].onTick) APPS[appIndex].onTick(now);
  }

  powerTick(now);
  delay(10);
}
