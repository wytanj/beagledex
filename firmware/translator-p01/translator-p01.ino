/*
 * translator-p01 — Phase 00 + 01 + 04 of the build plan.
 *
 *   Phase 00  ES8311 comes up, mic → speaker loopback is audible
 *   Phase 01  push-to-talk: hold BOOT to record into PSRAM, release to play back
 *   Phase 04  the AMOLED shows state, mic level and device info (ui.display)
 *
 * No network required. If secrets.h is present with WiFi credentials it will
 * also register with translator-console and ship its logs there, but audio
 * works either way — the point of this phase is to prove the audio path in
 * isolation, before anything else can be blamed for a failure.
 *
 * Board: Waveshare ESP32-S3-Touch-AMOLED-1.8, v2 panel — the display driver is
 * CO5300, not SH8601; pin_config.h carries the evidence.
 * Select "ESP32S3 Dev Module", PSRAM enabled, Flash 16MB.
 *
 * Requires es8311.c / es8311.h / es8311_reg.h beside this file — see README.md —
 * plus the "GFX Library for Arduino" library for the panel.
 */

#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <esp_mac.h>          // esp_read_mac / ESP_MAC_WIFI_STA
#include "pin_config.h"
#include <Arduino_GFX_Library.h>
#include <math.h>             // log10f, for the dBFS level meter

extern "C" {
#include "es8311.h"
}

#if __has_include("secrets.h")
  #include "secrets.h"
  #define HAVE_SECRETS 1
#endif

#ifdef HAVE_SECRETS
  #include <WiFi.h>
  #include <HTTPClient.h>
#endif

/* ── config ───────────────────────────────────────────────────────────────── */

static constexpr uint32_t SAMPLE_RATE  = AUDIO_SAMPLE_RATE;   // 16 kHz
static constexpr uint32_t MAX_SECONDS  = 30;                  // hard cap, see plan

/* The ES8311 is a mono codec, but the I2S frame still carries two slots.
 * Reading MONO against a two-slot frame misaligns every sample — it plays back
 * as speech-shaped noise with no intelligible voice. Match the vendor's proven
 * STEREO config and treat the buffer as interleaved L,R frames. */
static constexpr size_t   CHANNELS     = 2;
static constexpr size_t   MAX_SAMPLES  = SAMPLE_RATE * CHANNELS * MAX_SECONDS;
static constexpr size_t   CHUNK        = 512;                 // int16 samples (both slots)

static const char *FW_VERSION = "0.3.0";
static const char *FEATURES   = "\"audio.loopback\",\"audio.ptt\",\"ui.display\"";

/* ── state ────────────────────────────────────────────────────────────────── */

I2SClass    i2s;
es8311_handle_t codec = nullptr;

static int16_t *buffer  = nullptr;   // lives in PSRAM
static size_t   recorded = 0;
static char     deviceId[13] = {0};

/* Last measured capture levels, so the display can show what reportLevels()
 * already computed instead of walking the buffer a second time. */
static int32_t  lastPeak = 0;
static uint32_t lastRms  = 0;

/* ── logging: serial always, console when we have a network ───────────────── */

static void logLine(const char *level, const char *tag, const char *msg);

#define LOGI(tag, ...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); logLine("info",  tag, _b); } while (0)
#define LOGW(tag, ...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); logLine("warn",  tag, _b); } while (0)
#define LOGE(tag, ...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); logLine("error", tag, _b); } while (0)

/* ── phase 04: display ───────────────────────────────────────────────────────
 * CO5300 AMOLED, 368x448, over QSPI. Not SH8601 — pin_config.h has the evidence,
 * and the wrong driver here is a plausible, silent, black screen.
 *
 * ONE HARD RULE: never repaint while capturing. The audio buffer lives in PSRAM,
 * this board's PSRAM is quad rather than octal, and pushing pixels during a
 * capture is precisely the bandwidth contention that shows up as crackle. Every
 * repaint below therefore happens either side of a capture, never inside one.
 *
 * That is also why there is deliberately no live meter while recording: it would
 * be the one feature guaranteed to corrupt the recording it was displaying. The
 * meter shows the capture you just finished, which is the number you actually
 * want anyway — you cannot fix your mic technique mid-sentence.
 */
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED /* RST is behind a TCA9554, and unneeded */, 0,
    LCD_WIDTH, LCD_HEIGHT, 16 /* col_offset1 — this panel starts 16 in */, 0, 0, 0);

static bool haveDisplay = false;

/* Layout as named constants: every box has to agree with its neighbours, and
 * magic numbers sprinkled through draw calls drift apart the moment you edit. */
static constexpr int16_t PAD     = 12;
static constexpr int16_t HEAD_H  = 64;
static constexpr int16_t STATE_Y = 80;
static constexpr int16_t STATE_H = 112;
static constexpr int16_t METER_Y = 236;
static constexpr int16_t METER_H = 30;
static constexpr int16_t METER_W = LCD_WIDTH - 2 * PAD;
static constexpr int16_t INFO_Y  = 300;
static constexpr int16_t FOOT_Y  = 420;

static constexpr uint16_t C_BG     = RGB565(0, 0, 0);
static constexpr uint16_t C_CARD   = RGB565(22, 24, 28);
static constexpr uint16_t C_DIM    = RGB565(130, 138, 150);
static constexpr uint16_t C_FAINT  = RGB565(70, 76, 86);
static constexpr uint16_t C_ACCENT = RGB565(16, 148, 152);
static constexpr uint16_t C_READY  = RGB565(64, 208, 120);
static constexpr uint16_t C_REC    = RGB565(232, 72, 72);
static constexpr uint16_t C_PLAY   = RGB565(96, 168, 248);
static constexpr uint16_t C_WARN   = RGB565(240, 176, 64);

/* Peak sample → bar width, on a dBFS scale spanning -60 dB to full scale.
 * Linear would be useless: speech peaks land in the low hundreds out of 32767,
 * which is under 1% of the width. dBFS is also the vocabulary the rest of this
 * project already measures the mic in. */
static int16_t meterPx(int32_t peak) {
  if (peak <= 0) return 0;
  const float db = 20.0f * log10f((float)peak / 32767.0f);
  const float f  = (db + 60.0f) / 60.0f;
  if (f <= 0.0f) return 0;
  if (f >= 1.0f) return METER_W - 2;
  return (int16_t)(f * (METER_W - 2));
}

static void uiChrome() {
  gfx->fillScreen(C_BG);

  gfx->fillRect(0, 0, LCD_WIDTH, HEAD_H, C_ACCENT);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(PAD, 20);
  gfx->print("translator");
  gfx->setTextSize(1);
  gfx->setCursor(LCD_WIDTH - PAD - 30, 28);
  gfx->print(FW_VERSION);

  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(PAD, METER_Y - 26);
  gfx->print("mic level");
  gfx->drawRect(PAD, METER_Y, METER_W, METER_H, C_CARD);

  gfx->setTextSize(2);
  gfx->setTextColor(C_FAINT);
  gfx->setCursor(PAD, FOOT_Y);
  gfx->print("phase 00 + 01 + 04");
}

static void uiState(const char *label, const char *hint, uint16_t colour) {
  if (!haveDisplay) return;
  gfx->fillRoundRect(PAD, STATE_Y, LCD_WIDTH - 2 * PAD, STATE_H, 10, C_CARD);
  gfx->setTextColor(colour);
  gfx->setTextSize(4);
  gfx->setCursor(PAD + 16, STATE_Y + 24);
  gfx->print(label);
  gfx->setTextColor(C_DIM);
  gfx->setTextSize(2);
  gfx->setCursor(PAD + 16, STATE_Y + 76);
  gfx->print(hint);
}

static void uiMeter(int32_t peak, uint32_t rms) {
  if (!haveDisplay) return;

  gfx->fillRect(PAD + 1, METER_Y + 1, METER_W - 2, METER_H - 2, C_BG);
  const int16_t w     = meterPx(peak);
  const int16_t amber = meterPx(8230);    // -12 dBFS
  const int16_t red   = meterPx(23197);   //  -3 dBFS
  if (w > 0)     gfx->fillRect(PAD + 1, METER_Y + 1, (w < amber ? w : amber), METER_H - 2, C_READY);
  if (w > amber) gfx->fillRect(PAD + 1 + amber, METER_Y + 1, (w < red ? w : red) - amber, METER_H - 2, C_WARN);
  if (w > red)   gfx->fillRect(PAD + 1 + red, METER_Y + 1, w - red, METER_H - 2, C_REC);

  gfx->fillRect(PAD, METER_Y + METER_H + 8, METER_W, 18, C_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(PAD, METER_Y + METER_H + 8);
  if (peak > 0) gfx->printf("peak %ld  %.0f dBFS", (long)peak,
                            20.0f * log10f((float)peak / 32767.0f));
  else          gfx->print("no signal");
}

static void uiInfo(const char *link) {
  if (!haveDisplay) return;
  gfx->fillRect(PAD, INFO_Y, METER_W, 108, C_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  int16_t y = INFO_Y;
  gfx->setCursor(PAD, y); gfx->printf("device  %s", deviceId);                       y += 26;
  gfx->setCursor(PAD, y); gfx->printf("audio   %u Hz x%u", SAMPLE_RATE, (unsigned)CHANNELS); y += 26;
  gfx->setCursor(PAD, y); gfx->printf("buffer  %u s max", MAX_SECONDS);              y += 26;
  gfx->setCursor(PAD, y); gfx->printf("link    %s", link);
}

/* Short messages only — the hint line fits about 26 characters. */
static void uiFatal(const char *msg) { uiState("FAULT", msg, C_REC); }

static bool uiBegin() {
  if (!gfx->begin()) return false;
  haveDisplay = true;
  gfx->setBrightness(255);   // an AMOLED at brightness 0 is indistinguishable
                             // from a dead one, so never leave this implicit
  uiChrome();
  return true;
}

/* ── I2C inventory ────────────────────────────────────────────────────────────
 * Cheap insurance. This board ships in two revisions whose parts differ, and
 * assuming the v1 part list already cost us once — the panel turned out to be a
 * CO5300 where every note here said SH8601. So enumerate the bus and say what
 * actually answered, rather than trusting a datasheet for a board we may not
 * have. A bare address probe writes no register, so it disturbs nothing.
 *
 * Must run after Wire.begin(), which startCodec() does.
 */
static const char *i2cName(uint8_t a) {
  switch (a) {
    case 0x18: return "ES8311 codec";
    case 0x15: return "CST816/820 touch";
    case 0x38: return "FT3168 touch";
    case 0x34: return "AXP2101 PMIC";
    case 0x51: return "PCF85063 RTC";
    case 0x6A:
    case 0x6B: return "QMI8658 IMU";
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27: return "TCA9554 expander";
    default:   return "unidentified";
  }
}

static void scanI2C() {
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      LOGI("i2c", "0x%02X  %s", a, i2cName(a));
    }
  }
  if (!found) LOGW("i2c", "nothing answered on SDA=%d SCL=%d", IIC_SDA, IIC_SCL);
  else        LOGI("i2c", "%d device(s) on the bus", found);
}

/* ── codec bring-up ───────────────────────────────────────────────────────── */

static bool startCodec() {
  Wire.begin(IIC_SDA, IIC_SCL, 400000);

  // The amp is gated. Leave this low and playback is silent while every other
  // signal looks perfectly correct — the single biggest time-waster here.
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);

  // es8311_create takes a plain uint16_t address — there is no es8311_addr_t.
  codec = es8311_create(0, ES8311_ADDRRES_0);
  if (!codec) {
    LOGE("codec", "es8311_create failed — nothing answering at 0x%02X on SDA=%d SCL=%d",
         ES8311_ADDRRES_0, IIC_SDA, IIC_SCL);
    return false;
  }

  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256,
    .sample_frequency = SAMPLE_RATE,
  };

  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    LOGE("codec", "es8311_init failed");
    return false;
  }

  es8311_sample_frequency_config(codec, SAMPLE_RATE * 256, SAMPLE_RATE);
  es8311_voice_volume_set(codec, AUDIO_VOICE_VOLUME, nullptr);
  es8311_microphone_config(codec, false);          // false = analog mic, as fitted
  es8311_microphone_gain_set(codec, (es8311_mic_gain_t)AUDIO_MIC_GAIN);

  LOGI("codec", "ES8311 up @ %u Hz, vol %d, gain %d", SAMPLE_RATE, AUDIO_VOICE_VOLUME, AUDIO_MIC_GAIN);
  return true;
}

static bool startI2S() {
  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    LOGE("i2s", "begin failed on bclk=%d ws=%d dout=%d din=%d mclk=%d",
         I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
    return false;
  }
  LOGI("i2s", "standard mode, 16-bit, %u slots", CHANNELS);
  return true;
}

/* ── level metering ───────────────────────────────────────────────────────────
 * Distinguishes the failure modes without needing ears:
 *   both channels ~0            → mic not capturing at all
 *   one channel signal, other 0 → mic is on a single slot (normal for ES8311)
 *   both channels loud + noisy  → frame misalignment, not real audio
 * RMS is what correlates with perceived loudness; peak catches clipping.
 */
/* Takes only primitives on purpose: the Arduino .ino preprocessor hoists
 * generated function prototypes above anything declared later in the file, so a
 * custom struct in a signature fails to compile with "does not name a type". */
static void reportLevels(const char *tag, const int16_t *s, size_t count) {
  const size_t frames = count / CHANNELS;
  if (!frames) { LOGW(tag, "no samples to measure"); return; }

  uint64_t sqL = 0, sqR = 0;
  int32_t pL = 0, pR = 0;
  for (size_t i = 0; i < frames; i++) {
    const int32_t l = s[i * CHANNELS], r = s[i * CHANNELS + 1];
    sqL += (uint64_t)(l * l);
    sqR += (uint64_t)(r * r);
    if (abs(l) > pL) pL = abs(l);
    if (abs(r) > pR) pR = abs(r);
  }
  const uint32_t rmsL = (uint32_t)sqrt((double)sqL / frames);
  const uint32_t rmsR = (uint32_t)sqrt((double)sqR / frames);

  lastPeak = pL;      // handed to the display; see phase 04
  lastRms  = rmsL;

  LOGI(tag, "levels  L peak %6ld rms %5lu  |  R peak %6ld rms %5lu",
       (long)pL, (unsigned long)rmsL, (long)pR, (unsigned long)rmsR);

  if (rmsL < 30 && rmsR < 30)
    LOGW(tag, "both channels near silent — mic is not reaching the ADC");
  else if (rmsL > 8000 && rmsR > 8000)
    LOGW(tag, "both channels very hot — looks like frame misalignment, not audio");
  else
    LOGI(tag, "one channel carrying signal — normal for a mono codec");
}

/* ── half-duplex: capture and playback are never simultaneous ─────────────────
 * The mic and speaker sit centimetres apart in a sealed case and the ES8311 is
 * a mono codec with no echo cancellation. A live mic→speaker loopback therefore
 * cannot work: it saturates to full scale within milliseconds and comes out as
 * loud static. Measured on this board at rms 27319 of 32767 — 83% of full
 * scale, where speech is 1–10%.
 *
 * So the codec output is muted while capturing and only unmuted to play back.
 * This is not a workaround; it is the interaction model the hardware supports,
 * and it is what push-to-talk translation needs anyway.
 */
static void outputMuted(bool mute) {
  es8311_voice_mute(codec, mute);
  digitalWrite(PA, mute ? LOW : HIGH);   // gate the amplifier too, not just the DAC
}

/* ── phase 00: capture, measure, then play back ───────────────────────────── */

static void captureSelfTest(uint32_t ms) {
  const size_t want = (size_t)(SAMPLE_RATE * CHANNELS * ms / 1000);
  LOGI("p00", "capturing %u ms with output muted — say something", ms);

  outputMuted(true);
  flushInput();
  size_t n = 0;
  while (n < want) {
    const size_t room = want - n;
    const size_t req  = (room < CHUNK ? room : CHUNK) * sizeof(int16_t);
    const size_t got  = i2s.readBytes((char *)(buffer + n), req);
    if (got == 0) break;
    n += got / sizeof(int16_t);
  }
  recorded = n;

  LOGI("p00", "captured %.2f s (%u samples)", n / (float)CHANNELS / SAMPLE_RATE, n);
  reportLevels("p00", buffer, n);

  LOGI("p00", "playing it back — you should hear yourself now");
  outputMuted(false);
  playback();
}

/* ── phase 01: push to talk ───────────────────────────────────────────────── */

/* The RX DMA keeps filling while we sit idle, so the first read after a button
 * press returns audio from *before* the press — measured as 1.55 s of audio
 * captured in 1.47 s of wall time, i.e. ~80 ms of pre-roll. Harmless for a
 * local playback test, but Phase 03 sends these buffers to speech-to-text and
 * the leading content should be what the user actually asked to send. */
static void flushInput() {
  int16_t scratch[CHUNK];
  int guard = 0;
  while (i2s.available() > 0 && guard++ < 64) {
    i2s.readBytes((char *)scratch, sizeof scratch);
  }
}

static void record() {
  recorded = 0;
  flushInput();
  const uint32_t began = millis();
  LOGI("p01", "recording…");

  while (digitalRead(BOOT_BUTTON) == LOW && recorded < MAX_SAMPLES) {
    const size_t room  = MAX_SAMPLES - recorded;
    const size_t want  = (room < CHUNK ? room : CHUNK) * sizeof(int16_t);
    const size_t got   = i2s.readBytes((char *)(buffer + recorded), want);
    recorded += got / sizeof(int16_t);
  }

  const float secs = recorded / (float)CHANNELS / SAMPLE_RATE;
  if (recorded >= MAX_SAMPLES) {
    LOGW("p01", "hit the %u s cap — stopping capture", MAX_SECONDS);
  }
  LOGI("p01", "captured %.2f s (%u samples) in %u ms", secs, recorded, millis() - began);
  reportLevels("p01", buffer, recorded);
}

static void playback() {
  if (!recorded) { LOGW("p01", "nothing captured"); return; }
  LOGI("p01", "playing back %.2f s", recorded / (float)CHANNELS / SAMPLE_RATE);
  for (size_t i = 0; i < recorded; i += CHUNK) {
    const size_t n = (recorded - i < CHUNK ? recorded - i : CHUNK);
    i2s.write((uint8_t *)(buffer + i), n * sizeof(int16_t));
  }
  LOGI("p01", "playback done");
}

/* ── optional: console reporting ──────────────────────────────────────────── */

#ifdef HAVE_SECRETS
static bool online = false;

static bool post(const char *path, const String &body) {
  if (!online) return false;
  HTTPClient http;
  http.begin(String(CONSOLE_URL) + path);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  const int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[wifi] joining %s", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print('.'); }
  Serial.println();

  online = WiFi.status() == WL_CONNECTED;
  if (!online) { Serial.println("[wifi] no join — carrying on offline, audio still works"); return; }
  Serial.printf("[wifi] %s  rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  String body = "{\"mac\":\"" + WiFi.macAddress() + "\",\"chip\":\"ESP32-S3\",\"revision\":\"v0.2\""
                ",\"flashBytes\":16777216,\"psramBytes\":8388608"
                ",\"fwVersion\":\"" + FW_VERSION + "\",\"activeFeatures\":[" + FEATURES + "]}";
  Serial.println(post("/api/devices/register", body) ? "[console] registered" : "[console] register failed");
}

static void heartbeat() {
  String body = "{\"rssi\":" + String(WiFi.RSSI()) +
                ",\"uptimeSec\":" + String(millis() / 1000) +
                ",\"fwVersion\":\"" + FW_VERSION + "\"}";
  post((String("/api/devices/") + deviceId + "/heartbeat").c_str(), body);
}
#endif

static void logLine(const char *level, const char *tag, const char *msg) {
  Serial.printf("[%s/%s] %s\n", level, tag, msg);
#ifdef HAVE_SECRETS
  String body = String("{\"deviceId\":\"") + deviceId + "\",\"lines\":[{\"level\":\"" + level +
                "\",\"tag\":\"" + tag + "\",\"msg\":\"" + msg + "\"}]}";
  post("/api/logs", body);
#endif
}

/* ── setup / loop ─────────────────────────────────────────────────────────── */

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.printf("\n\ntranslator-p01 %s — phase 00 + 01 + 04\n", FW_VERSION);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(deviceId, sizeof deviceId, "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("device %s\n", deviceId);

  /* Display first, so that everything after this point has somewhere to report
   * a failure other than a serial line nobody may be watching. */
  if (uiBegin()) LOGI("ui", "CO5300 %dx%d up over QSPI", LCD_WIDTH, LCD_HEIGHT);
  else           LOGW("ui", "no panel this boot — carrying on, audio needs none of it");
  uiState("BOOT", "bringing up audio", C_PLAY);

  buffer = (int16_t *)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
  if (!buffer) {
    Serial.println("FATAL: ps_malloc failed — is PSRAM enabled in the board options?");
    uiFatal("PSRAM not enabled");
    while (true) delay(1000);
  }
  Serial.printf("buffer %u KB in PSRAM (%u s max)\n",
                (MAX_SAMPLES * sizeof(int16_t)) / 1024, MAX_SECONDS);

#ifdef HAVE_SECRETS
  connectWiFi();
  uiInfo(online ? "console" : "serial only");
#else
  Serial.println("no secrets.h — offline mode, serial logging only");
  uiInfo("serial only");
#endif

  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  if (!startCodec() || !startI2S()) {
    LOGE("boot", "audio bring-up failed — stopping so the failure is unambiguous");
    uiFatal("audio bring-up failed");
    while (true) delay(1000);
  }

  scanI2C();      // Wire is up by now; startCodec() opened it

  uiState("SELF TEST", "capturing 3 s, muted", C_PLAY);
  captureSelfTest(3000);
  uiMeter(lastPeak, lastRms);

  LOGI("boot", "ready — hold BOOT to record, release to hear it back");
  uiState("READY", "hold BOOT to talk", C_READY);
}

void loop() {
  static uint32_t lastBeat = 0;

  if (digitalRead(BOOT_BUTTON) == LOW) {
    delay(30);                                    // debounce
    if (digitalRead(BOOT_BUTTON) != LOW) return;

    // Both repaints below sit outside the capture window on purpose — see the
    // phase 04 note. Nothing touches the panel between here and release.
    uiState("RECORDING", "release to play back", C_REC);
    outputMuted(true);                                   // no feedback path while capturing
    record();
    while (digitalRead(BOOT_BUTTON) == LOW) delay(10);   // wait for release
    outputMuted(false);

    uiState("PLAYING", "", C_PLAY);
    uiMeter(lastPeak, lastRms);
    playback();
    uiState("READY", "hold BOOT to talk", C_READY);
  }

#ifdef HAVE_SECRETS
  if (online && millis() - lastBeat > 60000) { lastBeat = millis(); heartbeat(); }
#endif

  delay(10);
}
