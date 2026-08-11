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
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "pin_config.h"
#include "shell_types.h"   // Gesture + App — must arrive via a header, see that file

extern "C" {
#include "es8311.h"
}

static const char *FW_VERSION = "0.15.0";

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

/* Console green and orange, on black. Two hues only — everything is either the
 * green (normal, idle, good) or the orange (attention, active, wrong), which on an
 * AMOLED also means most of the panel stays unlit and cheap. */
static constexpr uint16_t C_BG     = RGB565(0, 0, 0);
static constexpr uint16_t C_CARD   = RGB565(8, 28, 14);      // near-black green tint
static constexpr uint16_t C_DIM    = RGB565(88, 176, 112);   // muted green, body text
static constexpr uint16_t C_FAINT  = RGB565(36, 88, 52);     // dark green, hints
static constexpr uint16_t C_ACCENT = RGB565(0, 232, 100);    // the green
static constexpr uint16_t C_OK     = RGB565(0, 232, 100);
static constexpr uint16_t C_COOL   = RGB565(168, 255, 192);  // pale green, transient states
static constexpr uint16_t C_WARN   = RGB565(255, 168, 32);   // the orange
static constexpr uint16_t C_HOT    = RGB565(255, 112, 0);    // deeper orange: recording, faults
static constexpr uint16_t C_AOD    = RGB565(0, 132, 56);     // dim green for the always-on face

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
/* AWAKE → DIM → AOD, and OFF only on an explicit lock (or a long AOD timeout).
 *
 * The always-on face is the one thing this panel is uniquely good at and the
 * previous design threw away: per-pixel emission means a handful of dim digits on
 * black costs almost nothing, where an LCD would still be burning a backlight for
 * the same picture. So the resting state now shows the time instead of nothing.
 *
 * OFF still exists, because "off" should mean off when you ask for it. */
enum PowerState : uint8_t { PWR_AWAKE, PWR_DIM, PWR_AOD, PWR_OFF };

static PowerState powerState   = PWR_AWAKE;
static uint32_t   lastActivity = 0;
static uint32_t   dimAfterMs   = 15000;
static uint32_t   aodAfterMs   = 45000;   // → always-on face
static uint32_t   offAfterMs   = 0;       // 0 = rest at AOD and never go fully dark
static bool       aodEnabled   = true;
static uint8_t    brightFull   = 255;
static uint8_t    brightDim    = 40;
static uint8_t    aodBright    = 14;      // enough to read in a dim room, little else

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

/* ── network: provisioned over serial, stored in NVS ─────────────────────────
 * Credentials arrive as host commands and live in NVS, which was 840 KB of
 * otherwise unused partition. Deliberately NOT in secrets.h — that means a
 * rebuild to change networks and is one careless commit from being public — and
 * deliberately NOT in config/devices/*.json, because that file is tracked in git
 * and a WiFi password has no business in version control.
 *
 *   npm run push wifi "<ssid>" "<pass>"
 *   npm run push console http://<host>:3000
 *   npm run push token <DEVICE_TOKEN>
 *
 * NVS is unencrypted, so these are recoverable from a flash dump. Acceptable for
 * a device living on your own home network; it would not be for one carrying
 * anybody else's credentials.
 *
 * THE RADIO IS TIED TO THE SCREEN. WiFi costs on the order of 80 mA, which would
 * comfortably undo the sleep work — this thing already only manages eight hours.
 * So the radio comes up when the screen does and goes down when it sleeps, which
 * is a decent proxy for "is anyone using it". A capture that arrives before the
 * join completes waits for it rather than failing.
 */
static Preferences prefs;
static String netSsid, netPass, netConsole, netToken;
static bool   online = false;

static void netLoad() {
  prefs.begin("net", true);
  netSsid    = prefs.getString("ssid", "");
  netPass    = prefs.getString("pass", "");
  netConsole = prefs.getString("console", "");
  netToken   = prefs.getString("token", "");
  prefs.end();
}

static void netSave(const char *k, const String &v) {
  prefs.begin("net", false);
  prefs.putString(k, v);
  prefs.end();
}

/* Start a join and return immediately. Waking the screen must not stall on the
 * radio — a frozen UI while WiFi negotiates would be a worse bug than being
 * briefly offline. loop() notices when it lands. */
static void netKick() {
  if (!netSsid.length() || WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(netSsid.c_str(), netPass.c_str());
}

static bool netConnect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) { online = true; return true; }
  if (!netSsid.length()) { LOGW("net", "no ssid stored — run: npm run push wifi"); return false; }

  netKick();
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(120);

  online = WiFi.status() == WL_CONNECTED;
  if (online) LOGI("net", "%s  ip %s  rssi %d  in %lu ms", netSsid.c_str(),
                   WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)(millis() - t0));
  else        LOGW("net", "join failed for %s — the rest of the watch is unaffected", netSsid.c_str());
  return online;
}

static void netSleep() {
  if (!online && WiFi.getMode() == WIFI_OFF) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  online = false;
  LOGI("net", "radio off with the screen");
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
static FaceDesc face = { LCD_WIDTH, LCD_HEIGHT, 40, 180, 7, RGB565(0, 232, 100), true, 3000 };

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
/* Two selections and one button, exactly as specified — and no third control,
 * because the direction is decided by the console from the detected language.
 * Say the far-side language and it comes back in yours. A toggle would be one
 * more thing to fumble mid-sentence. */
struct Lang { const char *code; const char *label; };
static const Lang LANGS[] = {
  {"en", "English"},  {"ja", "Japanese"}, {"zh", "Chinese"},    {"ko", "Korean"},
  {"es", "Spanish"},  {"fr", "French"},   {"de", "German"},     {"th", "Thai"},
  {"vi", "Vietnamese"}, {"id", "Indonesian"}, {"ms", "Malay"},  {"ta", "Tamil"},
};
static constexpr int LANG_COUNT = sizeof(LANGS) / sizeof(LANGS[0]);
static int langA = 0, langB = 1;                  // A = you, B = the other speaker

static const char *langLabel(const char *code) {
  for (int i = 0; i < LANG_COUNT; i++) if (!strcmp(LANGS[i].code, code)) return LANGS[i].label;
  return code;
}

static char     trStatusText[20] = "READY";
static uint16_t trStatusColour   = C_OK;
static char     trTranscript[200] = {0};
static char     trTranslation[200] = {0};
static char     trNote[72] = {0};

/* Last HTTP outcome, kept for `>last`. */
static int      lastHttpCode  = 0;
static size_t   lastSentBytes = 0;
static size_t   lastBodyLen   = 0;
static char     lastBodyHead[120] = {0};

static constexpr int16_t TR_ROW_H = 56;      // the two "you/them" cards, enlarged
static constexpr int16_t TR_TEXT_Y = BODY_Y + 2 * TR_ROW_H + 44;   // result / rendered-reply area

/* Full-screen language grid. Cycling forward through twelve languages meant a
 * careless tap had to be walked all the way around; a grid is direct selection,
 * with targets big enough not to mis-tap in the first place. -1 = closed, else
 * the side being chosen (0 = you, 1 = them). */
static int pickerFor = -1;
static constexpr int16_t PK_TOP    = BODY_Y + 30;
static constexpr int16_t PK_CELL_H = 47;
static constexpr int16_t PK_CELL_W = (LCD_WIDTH - 2 * PAD - 8) / 2;   // two columns, small gap
static constexpr int16_t PK_X1     = PAD + PK_CELL_W + 8;

/* The built-in GFX font is ASCII. Japanese, Chinese, Korean and Thai would render
 * as mojibake, so the device speaks those replies instead of drawing them and the
 * screen shows only what it can. */
static bool renderable(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p >= 0x80) return false;
  return true;
}

/* The two selector cards. Bigger now, with the language in the accent colour and
 * a chevron to say they are tappable. */
static void trPaintRows() {
  for (int i = 0; i < 2; i++) {
    const int16_t y = BODY_Y + i * TR_ROW_H;
    gfx->fillRoundRect(PAD, y, BODY_W, TR_ROW_H - 8, 8, C_CARD);
    gfx->setTextSize(2);
    gfx->setTextColor(C_DIM);
    gfx->setCursor(PAD + 12, y + 16);
    gfx->print(i == 0 ? "you" : "them");
    gfx->setTextSize(3);
    gfx->setTextColor(C_ACCENT);
    gfx->setCursor(PAD + 84, y + 12);
    gfx->print(LANGS[i == 0 ? langA : langB].label);
    gfx->setTextSize(2);
    gfx->setTextColor(C_FAINT);
    gfx->setCursor(LCD_WIDTH - PAD - 22, y + 16);
    gfx->print(">");
  }
}

/* The picker: a 2-column grid of every language, current one filled. Tap to
 * choose; tap the title to back out unchanged. */
static void pickerPaint() {
  clearBody();
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(PAD, BODY_Y + 4);
  gfx->printf("%s speaks:  (tap here to cancel)", pickerFor == 0 ? "you" : "them");

  const int cur = pickerFor == 0 ? langA : langB;
  for (int i = 0; i < LANG_COUNT; i++) {
    const int16_t x = (i & 1) ? PK_X1 : PAD;
    const int16_t y = PK_TOP + (i / 2) * PK_CELL_H;
    const bool sel = (i == cur);
    gfx->fillRoundRect(x, y, PK_CELL_W, PK_CELL_H - 7, 6, sel ? C_ACCENT : C_CARD);
    gfx->setTextSize(2);
    gfx->setTextColor(sel ? C_BG : C_DIM);
    gfx->setCursor(x + 12, y + 12);
    gfx->print(LANGS[i].label);
  }
}

static void trPaintStatus() {
  const int16_t y = BODY_Y + 2 * TR_ROW_H + 6;
  gfx->fillRect(PAD, y, BODY_W, 30, C_BG);
  gfx->setTextSize(3);
  gfx->setTextColor(trStatusColour);
  gfx->setCursor(PAD, y);
  gfx->print(trStatusText);

  gfx->setTextSize(1);
  gfx->setTextColor(online ? C_OK : C_WARN);
  gfx->setCursor(LCD_WIDTH - PAD - 60, y + 8);
  gfx->print(online ? "online" : "offline");
}

static void trPaintText() {
  const int16_t y = BODY_Y + 2 * TR_ROW_H + 44;
  gfx->fillRect(PAD, y, BODY_W, BODY_Y + BODY_H - y, C_BG);
  int16_t cy = y;

  /* This device speaks its replies, so the screen is confirmation, not output. It
   * shows only what the ASCII font can actually draw — the near-side transcript
   * when that side is Latin — and never tries to render the foreign reply, because
   * you just heard it. A "this script needs rendering" warning over a translation
   * the speaker already heard aloud is noise pretending to be an error. */
  if (trTranscript[0] && renderable(trTranscript)) {
    drawWrapped(trTranscript, PAD, cy, y + 80, C_DIM);
    cy += 84;
  }
  // A Latin-script reply (Spanish, French, …) is worth showing too; a non-Latin
  // one was spoken and is deliberately not drawn.
  if (trTranslation[0] && renderable(trTranslation)) {
    drawWrapped(trTranslation, PAD, cy, BODY_Y + BODY_H - 14, C_ACCENT);
  } else if (trNote[0]) {
    drawWrapped(trNote, PAD, cy, BODY_Y + BODY_H - 14, C_FAINT);
  }
}

static void askStatus(const char *s, uint16_t colour) {   // kept: the shell calls this
  snprintf(trStatusText, sizeof trStatusText, "%s", s);
  trStatusColour = colour;
  trPaintStatus();
}

static void askEnter() {
  clearBody();
  if (pickerFor >= 0) { pickerPaint(); return; }
  trPaintRows();
  if (!haveAudio) { snprintf(trStatusText, sizeof trStatusText, "NO AUDIO"); trStatusColour = C_HOT; }
  trPaintStatus();
  if (!trTranscript[0] && !trTranslation[0] && !trNote[0])
    snprintf(trNote, sizeof trNote, "tap a language, or hold BOOT to talk");
  trPaintText();
}

static void askTick(uint32_t) {}

static void askGesture(Gesture g) {
  if (g != G_TAP) return;

  if (pickerFor >= 0) {                              // choosing a language
    if (tapY < PK_TOP) { pickerFor = -1; askEnter(); return; }   // title = cancel
    const int col = tapX < PK_X1 ? 0 : 1;
    const int row = (tapY - PK_TOP) / PK_CELL_H;
    const int idx = row * 2 + col;
    if (idx >= 0 && idx < LANG_COUNT) {
      if (pickerFor == 0) langA = idx; else langB = idx;
      LOGI("translate", "pair %s-%s", LANGS[langA].code, LANGS[langB].code);
    }
    pickerFor = -1;
    askEnter();
    return;
  }

  // main view: a card opens its picker; a tap below clears the last result
  if (tapY < BODY_Y + TR_ROW_H)          { pickerFor = 0; askEnter(); }
  else if (tapY < BODY_Y + 2 * TR_ROW_H) { pickerFor = 1; askEnter(); }
  else {
    trTranscript[0] = trTranslation[0] = 0;
    snprintf(trNote, sizeof trNote, "hold BOOT, speak, release");
    trPaintText();
  }
}

/* Pull one JSON string field out without dragging in a parser. The device only
 * ever talks to our own endpoint, whose shape is fixed in the same repo. */
/* Tolerate whitespace around the colon. The strict form `"name":"` cost an
 * embarrassing amount of debugging: Nitro pretty-prints JSON in dev, so the wire
 * actually carries `"transcript": "…"` with a space, the match failed, and an
 * empty transcript is indistinguishable from silence — so a working pipeline
 * reported a dead microphone. Worse, the strict version would have started
 * working in production, where the JSON is compact. */
static bool jsonField(const String &src, const char *name, char *out, size_t cap) {
  const String key = String("\"") + name + "\"";
  const int len = (int)src.length();
  int i = src.indexOf(key);
  if (i < 0) return false;
  i += (int)key.length();

  while (i < len && isspace((unsigned char)src[i])) i++;
  if (i >= len || src[i] != ':') return false;
  i++;
  while (i < len && isspace((unsigned char)src[i])) i++;
  if (i >= len || src[i] != '"') return false;
  i++;

  const int e = src.indexOf('"', i);
  if (e < 0) return false;
  snprintf(out, cap, "%s", src.substring(i, e).c_str());
  return true;
}

/* Percent-decode a header value in place-ish into out. The console sends text
 * URL-encoded because HTTP headers are Latin-1 and the transcript can be UTF-8.
 * Only the transcript is shown (it is the near-side language, so it renders); the
 * translation is spoken, not drawn, which is the whole point of this path. */
static void urlDecode(const String &in, char *out, size_t cap) {
  size_t o = 0;
  for (int i = 0; i < (int)in.length() && o < cap - 1; i++) {
    const char c = in[i];
    if (c == '%' && i + 2 < (int)in.length()) {
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return 0;
      };
      out[o++] = (char)((hex(in[i + 1]) << 4) | hex(in[i + 2]));
      i += 2;
    } else if (c == '+') {
      out[o++] = ' ';
    } else {
      out[o++] = c;
    }
  }
  out[o] = 0;
}

/* Play 2-slot 16-bit PCM straight to I2S. This is the first output path in the
 * shell — until now the amp was gated 24/7 because nothing spoke. So it unmutes
 * and raises PA around the write and puts both back afterwards, leaving the codec
 * exactly as it found it: idle, muted, amp off, drawing nothing. Same half-duplex
 * discipline as capture, in the other direction. */
static void playPcm(const int16_t *pcm, size_t samples) {
  audioBusy = true;                       // no repaints while I2S streams from PSRAM
  es8311_voice_mute(codec, false);
  digitalWrite(PA, HIGH);
  delay(8);                               // let the amp settle before the first sample
  for (size_t i = 0; i < samples; i += CHUNK) {
    const size_t n = (samples - i < CHUNK) ? samples - i : CHUNK;
    i2s.write((uint8_t *)(pcm + i), n * sizeof(int16_t));
  }
  es8311_voice_mute(codec, true);         // back to the idle-muted state
  digitalWrite(PA, LOW);
  audioBusy = false;
}

static void askCapture(float secs) {
  trTranscript[0] = trTranslation[0] = 0;
  if (pickerFor >= 0) { pickerFor = -1; clearBody(); trPaintRows(); }  // results need the main view

  /* Distinguish "you let go too early" from "the microphone heard nothing". Both
   * used to say SILENT, which sent me hunting a hardware fault that did not
   * exist — the capture was simply 0.45 s long. Cheaper than a round trip, too. */
  if (secs < 0.6f) {
    askStatus("TOO SHORT", C_WARN);
    snprintf(trNote, sizeof trNote, "keep holding BOOT while you speak (%.1f s)", secs);
    trPaintText();
    LOGW("translate", "capture only %.2f s — not sent", secs);
    return;
  }

  askStatus("SENDING", C_COOL);
  snprintf(trNote, sizeof trNote, "%.1f s uploading...", secs);
  trPaintText();

  if (!netConsole.length()) {
    askStatus("NO HOST", C_HOT);
    snprintf(trNote, sizeof trNote, "npm run push console http://host:3000");
    trPaintText();
    return;
  }
  // The join may still be in flight if the button was pressed the instant the
  // screen woke, so wait for it here rather than failing the capture.
  if (!netConnect(8000)) {
    askStatus("OFFLINE", C_HOT);
    snprintf(trNote, sizeof trNote, "no wifi — npm run push wifi");
    trPaintText();
    return;
  }

  const uint32_t t0 = millis();
  String url = netConsole + "/api/translate?pair=" + LANGS[langA].code + "-"
             + LANGS[langB].code + "&channels=2&speak=1";
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/octet-stream");
  if (netToken.length()) http.addHeader("Authorization", String("Bearer ") + netToken);

  /* The reply text rides in headers, so we never parse a body we are about to
   * play. Ask HTTPClient to keep the ones we need. */
  static const char *keep[] = { "X-Transcript", "X-Translation", "X-Target",
                                "X-Audio-Format", "X-Audio-Error", "X-Text-W", "X-Text-H" };
  http.collectHeaders(keep, 7);

  /* The default 5 s covers none of this: a capture is a few hundred KB up, then
   * the console spends ~1.5 s on STT and translate and another ~1 s synthesising
   * speech, then streams the audio back down. Timing out mid-reply used to look
   * exactly like silence. */
  http.setTimeout(30000);

  const size_t sent = recorded * sizeof(int16_t);
  const int code = http.POST((uint8_t *)buffer, sent);

  lastHttpCode = code;
  lastSentBytes = sent;

  if (code != 200) {
    http.end();
    askStatus("ERROR", C_HOT);
    snprintf(trNote, sizeof trNote, "console said %d", code);
    trPaintText();
    LOGE("translate", "POST %s -> %d", url.c_str(), code);
    return;
  }

  urlDecode(http.header("X-Transcript"), trTranscript, sizeof trTranscript);
  urlDecode(http.header("X-Translation"), trTranslation, sizeof trTranslation);
  const String fmt = http.header("X-Audio-Format");
  char target[8] = {0};
  urlDecode(http.header("X-Target"), target, sizeof target);   // read before http.end()

  const bool clipped = lastPeak >= 32700;
  if (clipped) LOGW("mic", "capture clipped at peak %ld — lower the gain", (long)lastPeak);

  if (fmt.startsWith("pcm")) {
    /* Body is [text bitmap][audio pcm]. Both stream into the spent capture buffer
     * in turn: read the bitmap, blit it (this is the legible reply the ASCII font
     * can't draw), then read the audio over it and play. Reused sequentially, so
     * only the larger of the two needs to fit — and both do. */
    const size_t cap = MAX_SAMPLES * sizeof(int16_t);
    uint8_t *dst = (uint8_t *)buffer;
    WiFiClient *stream = http.getStreamPtr();

    const int tw = http.header("X-Text-W").toInt();
    const int th = http.header("X-Text-H").toInt();
    const size_t textBytes = (size_t)tw * th * 2;

    askStatus("SPEAKING", C_ACCENT);
    gfx->fillRect(PAD, TR_TEXT_Y, BODY_W, BODY_Y + BODY_H - TR_TEXT_Y, C_BG);

    // 1. the rendered reply
    if (tw > 0 && th > 0 && textBytes <= cap) {
      size_t got = 0;
      while (http.connected() && got < textBytes) {
        const size_t avail = stream->available();
        if (avail) {
          const size_t want = (textBytes - got < avail) ? textBytes - got : avail;
          const int r = stream->readBytes(dst + got, want);
          if (r <= 0) break;
          got += r;
        } else delay(2);
      }
      if (got == textBytes) gfx->draw16bitRGBBitmap(PAD, TR_TEXT_Y, (uint16_t *)buffer, tw, th);
    }

    // 2. the audio, over the same buffer
    const int total = http.getSize();
    const size_t audioTotal = (total > 0) ? (size_t)total - textBytes : 0;
    size_t got = 0;
    while (got < cap && (audioTotal == 0 || got < audioTotal)) {
      const size_t avail = stream->available();
      if (avail) {
        const size_t want = (cap - got < avail) ? cap - got : avail;
        const int r = stream->readBytes(dst + got, want);
        if (r <= 0) break;
        got += r;
      } else if (!http.connected()) {
        break;
      } else delay(2);
    }
    http.end();
    lastBodyLen = got;
    LOGI("translate", "http 200, sent %u B, text %dx%d, spoke %u B in %lu ms",
         (unsigned)sent, tw, th, (unsigned)got, (unsigned long)(millis() - t0));

    playPcm(buffer, got / sizeof(int16_t));

    // Status only — do NOT call trPaintText here, it would erase the blitted reply.
    askStatus(clipped ? "CLIPPED" : "READY", clipped ? C_WARN : C_OK);
    return;
  }

  /* No audio: either silence (nothing to say) or TTS failed. The words, if any,
   * are still in the headers. */
  http.end();
  lastBodyLen = 0;
  if (!trTranscript[0]) {
    askStatus("SILENT", C_WARN);
    snprintf(trNote, sizeof trNote, "nothing heard — try >mic");
  } else if (fmt == "none") {
    askStatus("NO VOICE", C_WARN);
    char err[80]; urlDecode(http.header("X-Audio-Error"), err, sizeof err);
    snprintf(trNote, sizeof trNote, "heard you, TTS failed: %s", err[0] ? err : "?");
  } else {
    askStatus("READY", C_OK);
    trNote[0] = 0;
  }
  trPaintText();
  LOGI("translate", "%.1f s (%lu ms) heard \"%s\"", secs,
       (unsigned long)(millis() - t0), trTranscript);
}

/* ── app: system ──────────────────────────────────────────────────────────── */

static const char *powerName() {
  switch (powerState) {
    case PWR_AWAKE: return "awake";
    case PWR_DIM:   return "dim";
    case PWR_AOD:   return "always-on";
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
  { "app.ask",    "Translate", "hold BOOT \xB7 tap a row",  askEnter, askTick, askGesture, askCapture, false },
  { "app.system", "System", "swipe down for settings",   sysEnter,   sysTick,   nullptr,     nullptr,    false },
};
static constexpr int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

/* ── shell chrome ─────────────────────────────────────────────────────────── */

static void drawStatus() {
  gfx->fillRect(0, 0, LCD_WIDTH, STATUS_H, C_CARD);
  gfx->setTextSize(2); gfx->setTextColor(C_ACCENT);
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

/* ── the always-on face ───────────────────────────────────────────────────────
 * Time only, dim, no chrome, redrawn once a minute.
 *
 * The position shifts a few pixels every minute, and that is not decoration: an
 * AMOLED ages per pixel, so digits held in one spot at one brightness for months
 * ghost permanently. Jitter spreads the wear over a band instead of etching it
 * into eight fixed glyph positions. It is cheap insurance against the one failure
 * mode this display technology has that an LCD does not.
 */
static constexpr int8_t AOD_JITTER[8][2] = {
  {0, 0}, {6, 0}, {6, 6}, {0, 6}, {-6, 6}, {-6, 0}, {-6, -6}, {0, -6},
};
static uint8_t aodStep = 0, aodLastMin = 255;

static void aodPaint(bool full) {
  if (full) { gfx->fillScreen(C_BG); aodLastMin = 255; }
  if (!full && rtc.min == aodLastMin) return;
  aodLastMin = rtc.min;

  static constexpr int16_t BX = 44, BY = 190, SZ = 6;
  // Clear generously so the previous jittered position goes with it.
  gfx->fillRect(BX - 14, BY - 14, 6 * SZ * 5 + 32, 8 * SZ + 28, C_BG);

  aodStep = (uint8_t)((aodStep + 1) & 7);
  gfx->setTextSize(SZ);
  gfx->setTextColor(C_AOD);
  gfx->setCursor(BX + AOD_JITTER[aodStep][0], BY + AOD_JITTER[aodStep][1]);
  if (rtc.valid) gfx->printf("%02u:%02u", rtc.hour, rtc.min);
  else           gfx->print("--:--");
}

static void wakeScreen() {
  if (!haveDisplay) return;
  const bool wasOff  = powerState == PWR_OFF;
  const bool wasDark = wasOff || powerState == PWR_AOD;
  powerState = PWR_AWAKE;
  gfx->setBrightness(brightFull);
  if (wasDark) {
    if (wasOff) gfx->displayOn();
    // Repaint everything: nothing was drawn while the panel was off, and
    // trusting a controller to have kept a framebuffer is how this board ended
    // up displaying a stale vendor frame for a day.
    gfx->fillScreen(C_BG);
    // A watch wakes to its face, not to wherever you last left it. Apps are
    // somewhere you go; the face is where the device lives.
    enterApp(0);
    netKick();          // radio follows the screen; non-blocking on purpose
    LOGI("pwr", "awake");
  }
  noteActivity();
}

static void powerTick(uint32_t now) {
  if (!haveDisplay) return;
  const uint32_t idle = now - lastActivity;

  if (powerState == PWR_AWAKE && dimAfterMs && idle > dimAfterMs) {
    powerState = PWR_DIM;
    gfx->setBrightness(brightDim);
    LOGI("pwr", "dim after %lu ms idle", (unsigned long)idle);

  } else if ((powerState == PWR_AWAKE || powerState == PWR_DIM) && aodAfterMs && idle > aodAfterMs) {
    // Radio goes either way: nobody is using it at rest, and 80 mA dwarfs the
    // handful of milliamps the dim digits cost.
    netSleep();
    if (aodEnabled) {
      powerState = PWR_AOD;
      gfx->setBrightness(aodBright);
      aodPaint(true);
      LOGI("pwr", "always-on face after %lu ms idle", (unsigned long)idle);
    } else {
      powerState = PWR_OFF;
      gfx->setBrightness(0);
      // Brightness alone is not enough: the controller keeps self-refreshing and
      // the panel keeps drawing. displayOff() is the part that saves the battery.
      gfx->displayOff();
      LOGI("pwr", "screen off after %lu ms idle", (unsigned long)idle);
    }

  } else if (powerState == PWR_AOD && offAfterMs && idle > offAfterMs) {
    powerState = PWR_OFF;
    gfx->setBrightness(0);
    gfx->displayOff();
    LOGI("pwr", "fully off after %lu ms idle", (unsigned long)idle);
  }
}

static void lockNow() {
  if (!haveDisplay || powerState == PWR_OFF) return;
  powerState = PWR_OFF;
  gfx->setBrightness(0);
  gfx->displayOff();
  netSleep();
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
  if (aodAfterMs) snprintf(b1, sizeof b1, "rest after   %lu s", (unsigned long)(aodAfterMs / 1000));
  else            snprintf(b1, sizeof b1, "rest after   never");
  snprintf(b2, sizeof b2, "always-on    %s", aodEnabled ? "on" : "off");
  snprintf(b3, sizeof b3, "lock now");
  labels[0] = b0; labels[1] = b1; labels[2] = b2; labels[3] = b3;

  for (int i = 0; i < 4; i++) {
    const int16_t y = QS_Y0 + i * QS_ROW_H;
    gfx->fillRoundRect(PAD, y, BODY_W, QS_ROW_H - 8, 8, C_CARD);
    gfx->setTextSize(2);
    gfx->setTextColor(i == 3 ? C_FAINT : C_ACCENT);   // audio row is read-only
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
    case 1: {                                   // how long before it rests
      if      (aodAfterMs == 30000)  aodAfterMs = 60000;
      else if (aodAfterMs == 60000)  aodAfterMs = 300000;
      else if (aodAfterMs == 300000) aodAfterMs = 0;
      else                           aodAfterMs = 30000;
      if (aodAfterMs && dimAfterMs > aodAfterMs / 2) dimAfterMs = aodAfterMs / 2;
      LOGI("pwr", "rest after %lu ms, dim after %lu ms",
           (unsigned long)aodAfterMs, (unsigned long)dimAfterMs);
      break;
    }
    case 2:                                     // always-on face on/off
      aodEnabled = !aodEnabled;
      LOGI("pwr", "always-on %s", aodEnabled ? "on" : "off");
      break;
    case 3: lockNow(); return;                  // no repaint: the screen is off
    default: return;
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
/* 400 ms, not 600: capture used to start so late that a natural press-and-speak
 * released before much audio existed. Still comfortably longer than a tap. */
static constexpr uint32_t BTN_HOLD_MS     = 400;
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
  else if (!strcmp(line, ">last")) {
    LOGI("last", "capture %u samples (%.2f s), peak %ld, rms %lu",
         (unsigned)recorded, recorded / (float)CHANNELS / SAMPLE_RATE,
         (long)lastPeak, (unsigned long)lastRms);
    LOGI("last", "http %d, sent %u B, reply %u B",
         lastHttpCode, (unsigned)lastSentBytes, (unsigned)lastBodyLen);
    LOGI("last", "reply head: %s", lastBodyHead[0] ? lastBodyHead : "(empty)");
    LOGI("last", "parsed transcript: %s", trTranscript[0] ? trTranscript : "(empty)");
  }
  else if (!strncmp(line, ">gain ", 6)) {
    /* ADC_REG16, the ADC scale: 0..7 = 0,6,12,18,24,30,36,42 dB. It is separate
     * from REG14's analog PGA, so raising it does not undo the "max PGA" the mic
     * config sets. Runtime-adjustable because finding the right value is a sweep,
     * and an 18-second reflash per step is a poor way to run one. */
    if (!haveAudio) { LOGW("mic", "audio not up"); return; }
    const int g = atoi(line + 6);
    if (g < 0 || g > 7) { LOGW("mic", "gain must be 0..7 (0,6,12,18,24,30,36,42 dB)"); return; }
    es8311_microphone_gain_set(codec, (es8311_mic_gain_t)g);
    LOGI("mic", "ADC scale %d (+%d dB); build default is %d (+%d dB)",
         g, g * 6, AUDIO_MIC_GAIN, AUDIO_MIC_GAIN * 6);
  }
  else if (!strncmp(line, ">vol ", 5)) {
    if (!haveAudio) { LOGW("mic", "audio not up"); return; }
    int v = atoi(line + 5);
    if (v < 0) v = 0; if (v > 100) v = 100;
    es8311_voice_volume_set(codec, v, nullptr);
    LOGI("mic", "DAC volume %d (build default %d); loudness mostly comes from host normalisation", v, AUDIO_VOICE_VOLUME);
  }
  else if (!strcmp(line, ">mic")) {
    /* Describe the raw ADC output numerically, which distinguishes the three
     * candidate faults that all look like "no audio":
     *   every sample identical      → the input is dead or unbiased
     *   small random values         → ADC alive, nothing acoustic reaching it
     *   L differs from R sometimes  → two independent channels, so the codec is
     *                                 not merely duplicating one slot
     * A DC offset also shows up as a non-zero mean, which points at bias rather
     * than at the microphone element. */
    if (!haveAudio) { LOGW("mic", "audio not up"); return; }
    int16_t scratch[CHUNK];
    int guard = 0;
    while (i2s.available() > 0 && guard++ < 64) i2s.readBytes((char *)scratch, sizeof scratch);

    const size_t want = SAMPLE_RATE * CHANNELS / 2;      // half a second
    size_t n = 0;
    while (n < want) {
      const size_t room = want - n;
      const size_t req  = (room < CHUNK ? room : CHUNK) * sizeof(int16_t);
      const size_t got  = i2s.readBytes((char *)(buffer + n), req);
      if (!got) break;
      n += got / sizeof(int16_t);
    }

    const size_t frames = n / CHANNELS;
    int32_t mn = 32767, mx = -32768;
    int64_t sum = 0;
    uint64_t sq = 0;
    size_t diff = 0;
    for (size_t i = 0; i < frames; i++) {
      const int16_t l = buffer[i * CHANNELS], r = buffer[i * CHANNELS + 1];
      if (l != r) diff++;
      if (l < mn) mn = l;
      if (l > mx) mx = l;
      sum += l;
      sq += (uint64_t)((int32_t)l * l);
    }
    LOGI("mic", "%u frames  min %ld  max %ld  mean %ld  rms %lu",
         (unsigned)frames, (long)mn, (long)mx,
         (long)(frames ? sum / (int64_t)frames : 0),
         (unsigned long)(frames ? (uint32_t)sqrt((double)sq / frames) : 0));
    LOGI("mic", "L!=R in %u/%u frames — %s", (unsigned)diff, (unsigned)frames,
         diff ? "independent channels" : "right slot mirrors left");
    char s[96];
    int p = 0;
    for (size_t i = 0; i < 10 && i < frames; i++)
      p += snprintf(s + p, sizeof(s) - p, "%d ", buffer[i * CHANNELS]);
    LOGI("mic", "first L samples: %s", s);
  }
  else if (!strncmp(line, ">aod", 4)) {
    // ">aod" rests immediately, ">aod off"/"on" toggles the feature. Saves waiting
    // 45 seconds every time you want to look at it.
    if      (!strcmp(line, ">aod off")) { aodEnabled = false; LOGI("pwr", "always-on off"); }
    else if (!strcmp(line, ">aod on"))  { aodEnabled = true;  LOGI("pwr", "always-on on"); }
    else {
      netSleep();
      powerState = PWR_AOD;
      gfx->setBrightness(aodBright);
      aodPaint(true);
      LOGI("pwr", "always-on face now");
    }
  }
  else if (!strncmp(line, ">sleep ", 7)) {
    const long s = atol(line + 7);            // seconds; 0 = never
    offAfterMs = (uint32_t)(s > 0 ? s * 1000 : 0);
    if (offAfterMs && dimAfterMs > offAfterMs / 2) dimAfterMs = offAfterMs / 2;
    LOGI("pwr", "sleep after %ld s", s);
  }
  else if (!strncmp(line, ">wifi ", 6)) {
    /* Split on the LAST space, so an SSID containing spaces still works while the
     * password (which must not contain one) stays intact. */
    char *arg = line + 6, *sp = strrchr(arg, ' ');
    if (!sp) { LOGW("cmd", "usage: >wifi <ssid> <password>"); return; }
    *sp = 0;
    netSsid = arg; netPass = sp + 1;
    netSave("ssid", netSsid); netSave("pass", netPass);
    // Length only. Never log the password itself — this line ends up in the
    // console's log store and in git-adjacent places.
    LOGI("net", "stored ssid %s, password %u chars", netSsid.c_str(), netPass.length());
    netConnect(12000);
    if (powerState != PWR_OFF && !qsOpen && appIndex == 1) trPaintStatus();
  }
  else if (!strncmp(line, ">console ", 9)) {
    netConsole = line + 9; netSave("console", netConsole);
    LOGI("net", "console %s", netConsole.c_str());
  }
  else if (!strncmp(line, ">token ", 7)) {
    netToken = line + 7; netSave("token", netToken);
    LOGI("net", "device token stored, %u chars", netToken.length());
  }
  else if (!strcmp(line, ">ping")) {
    /* Proves the whole device→console path — radio, URL, routing, firewall and
     * token — without needing anyone to hold the button and speak. On Windows the
     * usual failure is the host firewall silently dropping inbound 3000. */
    if (!netConsole.length()) { LOGW("net", "no console url — npm run push console"); return; }
    if (!netConnect(8000))    { LOGW("net", "offline — cannot ping"); return; }
    HTTPClient http;
    http.begin(netConsole + "/api/features");
    if (netToken.length()) http.addHeader("Authorization", String("Bearer ") + netToken);
    const uint32_t t0 = millis();
    const int code = http.GET();
    const int len  = http.getSize();
    http.end();
    if (code == 200) LOGI("net", "console reachable: %d, %d bytes, %lu ms",
                          code, len, (unsigned long)(millis() - t0));
    else             LOGE("net", "console unreachable: %d — host firewall on port 3000?", code);
  }
  else if (!strcmp(line, ">net")) {
    LOGI("net", "ssid=%s console=%s token=%s state=%s",
         netSsid.length() ? netSsid.c_str() : "(none)",
         netConsole.length() ? netConsole.c_str() : "(none)",
         netToken.length() ? "set" : "(none)", online ? "online" : "offline");
    if (online) LOGI("net", "ip %s rssi %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
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

  netLoad();
  if (netSsid.length()) { LOGI("net", "joining %s", netSsid.c_str()); netKick(); }
  else                  LOGI("net", "no credentials — npm run push wifi \"<ssid>\" \"<pass>\"");

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

  /* Resting on the always-on face: keep the clock honest and watch for a finger,
   * but run no apps and repaint nothing else. One draw a minute. */
  if (powerState == PWR_AOD) {
    if (touchPresent()) { wakeScreen(); return; }
    static uint32_t lastAod = 0;
    if (now - lastAod > 1000) { lastAod = now; rtcReadNow(); aodPaint(false); }
    delay(80);
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

  /* Notice the radio arriving or leaving. netKick() starts joins without waiting,
   * so this is where `online` actually becomes true. */
  static uint32_t lastNet = 0;
  static bool     netWas = false;
  if (now - lastNet > 1000) {
    lastNet = now;
    online = WiFi.status() == WL_CONNECTED;
    if (online != netWas) {
      netWas = online;
      if (online) LOGI("net", "online, ip %s rssi %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      else        LOGW("net", "offline");
      if (!qsOpen && appIndex == 1) trPaintStatus();
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
