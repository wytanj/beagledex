#pragma once
/*
 * pin_config.h — ESP32-S3-Touch-AMOLED-1.8 (Waveshare)
 *
 * RECONSTRUCTED. Every example in waveshareteam/ESP32-S3-Touch-AMOLED-1.8
 * does `#include "pin_config.h"`, but the header is not in the repository —
 * the GitHub clone does not compile without it. These values come from two
 * independent sources that agree:
 *
 *   · I2S pins: the vendor's own examples/arduino-v2/examples/15_ES8311
 *   · everything else: 78/xiaozhi-esp32,
 *     main/boards/waveshare/esp32-s3-touch-amoled-1.8/config.h
 *     (the factory firmware on this board IS Xiaozhi, so its map is the
 *      one the shipping image actually uses)
 *
 * The two sources overlap on MCLK/BCLK/WS/DIN/DOUT and match exactly, which
 * is the cross-check that makes the rest trustworthy.
 *
 * REVISIONS: two hardware variants exist (v1 and v2) with separate factory
 * images and separate xiaozhi board directories. Both configs were compared
 * and every pin below is IDENTICAL across them — I2S, I2C, PA, display QSPI
 * and BOOT. So this header is revision-independent and needs no v1/v2 guard.
 * Whatever differs between the variants, it is not these pins.
 */

/* ── I2C ──────────────────────────────────────────────────────────────────────
 * One bus. This is not a datasheet reading — it was enumerated live off this
 * unit by scanI2C() in translator-p01:
 *
 *   0x15  CST816/820 touch      ← v2 part, NOT the FT3168 of the v1 boards
 *   0x18  ES8311 codec
 *   0x20  TCA9554 GPIO expander (owns the panel reset)
 *   0x34  AXP2101 PMIC
 *   0x51  PCF85063 RTC
 *   0x6B  QMI8658 IMU
 *
 * Nothing answered at 0x38, so the FT3168 is simply not fitted here. That is a
 * second independent confirmation that this is a v2 board — the first being the
 * CO5300 panel — so assume v2 parts throughout unless a scan says otherwise.
 */
#define IIC_SDA                 15
#define IIC_SCL                 14

/* ── Audio: ES8311 mono codec ─────────────────────────────────────────────────
 * Analog mic on the codec's ADC, speaker off its DAC through a PA.
 * PA gates the amplifier — if it is low, playback is silent while every
 * other signal looks correct. This is the #1 time-waster on this board.
 */
#define I2S_MCK_IO              16
#define I2S_BCK_IO              9
#define I2S_WS_IO               45
#define I2S_DI_IO               10   /* codec → ESP32 (microphone) */
#define I2S_DO_IO               8    /* ESP32 → codec (speaker)    */
#define PA                      46   /* speaker amplifier enable   */

#define ES8311_ADDRRES_0        0x18 /* ES8311 default 7-bit I2C address */

/* 16 kHz is what speech-to-text wants and what the vendor example defaults to.
 * Xiaozhi runs this codec at 24 kHz — both work; 16 kHz means no resampling
 * anywhere between the microphone and the translation backend. */
#define AUDIO_SAMPLE_RATE       16000

/* ADC scale (ES8311 REG16): 0..7 = 0,6,12,18,24,30,36,42 dB. Separate from the
 * analog PGA in REG14, which es8311_microphone_config leaves at maximum.
 *
 * NOW 5 (30 dB). This was 2 (12 dB), chosen from an early measurement where gain
 * 3 peaked at -0.8 dBFS and clipped. Under those conditions that was right; under
 * current ones 12 dB left speech below the noise floor and transcription returned
 * empty strings every time.
 *
 * Swept on this unit with `npm run push raw ">mic"`, ambient room, no speech:
 *
 *   gain 2 (12 dB)   rms   36   peak  -142..121     unusable
 *   gain 5 (30 dB)   rms  300   peak -1106..1789    ~-25 dBFS, headroom for speech
 *   gain 7 (42 dB)   rms 1120   peak -4217..3581    ambient alone at -18 dBFS
 *
 * +18 dB of setting produced +18.4 dB of signal, so the whole chain is linear and
 * healthy — this was a level problem, not a fault.
 *
 * THEN MEASURED WITH ACTUAL SPEECH at gain 5: peak 32768, i.e. hard against the
 * rail, rms 1583. Clipped. Speech sits roughly 20 dB above ambient, so 30 dB of
 * gain is too much once someone actually talks — and the transcript showed it,
 * returning "just pick" for "just speak". Backed off to 4 (24 dB), which puts
 * speech peaks near -6 dBFS with ambient around rms 150.
 *
 * The original note about clipping hurting speech-to-text was right all along;
 * it was the 12 dB conclusion that had stopped fitting. Re-sweep with >gain and
 * check `>last` for peak 32768 whenever the enclosure or mic port changes. */
#define AUDIO_MIC_GAIN          4
#define AUDIO_VOICE_VOLUME      85

/* ── Display: CO5300 AMOLED over QSPI, 368 x 448 ──────────────────────────────
 * NOT SH8601, whatever the v1 documentation says. The two board revisions ship
 * different panel controllers, and the factory image dumped off THIS unit
 * (~/esp32-backups/28848590ba0c-full-16mb.bin) contains the string "co5300"
 * 33 times and "sh8601" not once. Arduino_GFX ships both drivers and the wrong
 * one gives a plausible, silent, black screen.
 *
 * CONFIRMED WORKING: Arduino_CO5300 over Arduino_ESP32QSPI, with
 * col_offset1 = 16 — this panel's visible area starts 16 columns in. See
 * firmware/panel-smoke. The pins below are shared by both revisions, so only
 * the driver identity was ever in question.
 *
 * Panel reset is NOT an ESP32 GPIO: a TCA9554 I2C expander owns it. Nothing
 * needs it, though — the panel comes up over QSPI alone, with no expander and
 * no AXP2101 call, so brightness is the only thing to remember to set.
 */
#define LCD_WIDTH               368
#define LCD_HEIGHT              448
#define LCD_CS                  12
#define LCD_SCLK                11
#define LCD_SDIO0               4
#define LCD_SDIO1               5
#define LCD_SDIO2               6
#define LCD_SDIO3               7

/* ── Buttons ──────────────────────────────────────────────────────────────────
 * There are exactly two, described by Waveshare as "onboard PWR and BOOT side
 * buttons, configurable for custom function development".
 *
 * BOOT — GPIO0, and the only one readable with digitalRead. It is a strapping
 * pin (held low at reset = download mode), but reading it as a button at runtime
 * is fine and standard. The shell maps it tap / double-tap / hold.
 *
 * PWR — NOT a GPIO. It is wired to the AXP2101's PWRKEY, so it can only be seen
 * over I2C in the PMIC's interrupt status. Waveshare document it as supporting
 * "customizable long-press/short-press behavior" but publish neither the
 * register nor the method, and it is not on the docs page. There is therefore no
 * #define for it here on purpose — inventing one would be a guess in the file
 * whose entire job is not guessing.
 *
 * To identify it: watch.ino's pmicIrqPeek() reads interrupt status bytes
 * 0x48..0x4A read-only and logs them on change. Press PWR and whichever bit
 * moves is the one it owns. Turning that into a usable button then needs a write
 * to clear the latch — which is not going in until the bit is confirmed, because
 * this chip owns the system rails.
 */
#define BOOT_BUTTON             0

/* ── microSD / TF slot — PINS UNKNOWN ─────────────────────────────────────────
 * The board has a TF slot and the plan needs it (audio.sdcache, phrase.offline),
 * but the pin assignment is genuinely not established:
 *
 *   · not in either source that gave us everything else
 *   · not on docs.waveshare.com's page for this board
 *   · the vendor's own 14_LVGL_SD_Test includes the pin_config.h their repo
 *     does not ship — the same gap that made this file necessary
 *
 * Do NOT guess these. A wrong SDMMC assignment drives pins that are already
 * doing something, and on this board that includes the codec and the panel.
 *
 * Leads, best first: the schematic PDF on files.waveshare.com for this board,
 * xiaozhi-esp32's board config, then the vendor SD example if its header ever
 * appears. Until then watch.ino's cache seam reports "no card" and callers
 * degrade instead of failing.
 */

/* ── Flash layout ─────────────────────────────────────────────────────────────
 * Recorded here because the app slot is NOT at the conventional 0x10000.
 * Writing an app image to 0x10000 on this board lands inside nvs.
 *
 * REPARTITIONED to clean A/B. The vendor layout — factory 5.5 MB, a single
 * ota_0 at 0x690000, and two SPIFFS partitions of vendor assets — is gone.
 * Source of truth is firmware/partitions-translator.csv; the table now on the
 * device was read back and is byte-identical to it.
 *
 *   0x009000  nvsfactory  nvs      200 KB   ← per-unit calibration. NEVER erase.
 *   0x03b000  nvs         nvs      840 KB
 *   0x10d000  otadata     ota        8 KB
 *   0x10f000  phy_init    phy        4 KB
 *   0x110000  ota_0       app        3 MB   ← flash app images HERE
 *   0x410000  ota_1       app        3 MB
 *   0x710000  storage     spiffs  8.94 MB
 *
 * Everything below 0x110000 kept its vendor offsets, so nvsfactory is
 * untouched. ota_0 begins exactly where factory used to, so the app flash
 * address is unchanged. With no factory partition, a blank otadata makes the
 * bootloader run ota_0 — the intended state after writing this table. The cost
 * is that two bad OTA images in a row need a USB recovery instead of a factory
 * fallback, which on this board is a ~6 s reflash.
 */
#define APP_PARTITION_OFFSET    0x110000
