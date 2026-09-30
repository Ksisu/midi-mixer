#include "calibration.h"
#include "inputs.h"
#include <Preferences.h>

CalData g_cal;
static bool s_valid = false;

static Preferences s_prefs;
static const char *NVS_NAMESPACE = "midimix";
static const char *NVS_KEY       = "cal";

bool calIsValid() { return s_valid; }

// --- helpers ---------------------------------------------------------------

static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

static uint32_t calCrc(const CalData &d) {
  return crc32((const uint8_t *)&d, sizeof(CalData) - sizeof(uint32_t));
}

// Both applyCal() and useTwoPoint() must agree on how much of each end is
// deadzone, so it is derived from the stored endpoints rather than stored.
// That also means the margin can be retuned without recalibrating.
static uint16_t endMargin(uint16_t lo, uint16_t hi) {
  if (hi <= lo) return 0;
  uint32_t span = (uint32_t)hi - (uint32_t)lo;
  uint32_t m = span * CAL_END_MARGIN_PCT / 100;
  if (m < CAL_END_MARGIN_MIN) m = CAL_END_MARGIN_MIN;
  if (m > span / 4) m = span / 4;  // never let the two margins meet
  return (uint16_t)m;
}

static void setLinearDefault(ChannelCal &c) {
  c.lo = 0;
  c.mid = ADC_MAX / 2;
  c.hi = ADC_MAX;
}

static bool channelSane(const ChannelCal &c) {
  if (c.hi <= c.lo) return false;
  if ((uint16_t)(c.hi - c.lo) < CAL_MIN_SPAN) return false;
  if (c.mid <= c.lo || c.mid >= c.hi) return false;
  return true;
}

// A half-throw too narrow to carry 64 MIDI steps cannot be linearized, but the
// channel is still perfectly usable end-to-end. Drop the mid point to the exact
// middle, which turns applyCal() into a straight two-point map. Far better than
// rejecting the channel and falling back to a full-scale default, which would
// squeeze a pot that stops at 900 into the bottom fifth of the MIDI range.
static bool useTwoPoint(ChannelCal &c) {
  // Judge the halves as applyCal() will actually see them: the deadzone is
  // taken off both ends first, so the room left for the taper correction is a
  // full margin smaller than the raw capture suggests.
  uint16_t m = endMargin(c.lo, c.hi);
  uint16_t lo = c.lo + m, hi = c.hi - m;
  if (c.mid > lo && c.mid < hi &&
      (uint16_t)(c.mid - lo) >= CAL_MIN_SEGMENT &&
      (uint16_t)(hi - c.mid) >= CAL_MIN_SEGMENT)
    return false;
  c.mid = (uint16_t)(((uint32_t)c.lo + (uint32_t)c.hi) / 2);
  return true;
}

static void loadFallback() {
  g_cal.magic = CAL_MAGIC;
  g_cal.version = CAL_VERSION;
  memset(g_cal.pad, 0, sizeof(g_cal.pad));
  for (uint8_t i = 0; i < NUM_ANALOG; i++) setLinearDefault(g_cal.ch[i]);
  g_cal.crc = calCrc(g_cal);
}

// --- storage ---------------------------------------------------------------

bool calLoad() {
  s_valid = false;
  loadFallback();

  CalData tmp;
  if (!s_prefs.begin(NVS_NAMESPACE, true)) {
    Serial.println("[cal] NVS namespace missing, calibration needed");
    return false;
  }
  size_t got = s_prefs.getBytes(NVS_KEY, &tmp, sizeof(tmp));
  s_prefs.end();

  if (got != sizeof(tmp)) {
    Serial.println("[cal] no stored calibration");
    return false;
  }
  if (tmp.magic != CAL_MAGIC || tmp.version != CAL_VERSION) {
    Serial.println("[cal] stored calibration has wrong magic/version");
    return false;
  }
  if (tmp.crc != calCrc(tmp)) {
    Serial.println("[cal] stored calibration failed CRC");
    return false;
  }
  for (uint8_t i = 0; i < NUM_ANALOG; i++) {
    if (!channelSane(tmp.ch[i])) {
      Serial.printf("[cal] stored channel %u is out of range\n", i + 1);
      return false;
    }
  }

  g_cal = tmp;
  s_valid = true;
  Serial.println("[cal] loaded calibration from NVS");
  return true;
}

bool calSave() {
  g_cal.magic = CAL_MAGIC;
  g_cal.version = CAL_VERSION;
  memset(g_cal.pad, 0, sizeof(g_cal.pad));
  g_cal.crc = calCrc(g_cal);

  if (!s_prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[cal] could not open NVS for writing");
    return false;
  }
  size_t written = s_prefs.putBytes(NVS_KEY, &g_cal, sizeof(g_cal));
  s_prefs.end();

  if (written != sizeof(g_cal)) {
    Serial.println("[cal] NVS write failed");
    return false;
  }
  Serial.println("[cal] calibration saved");
  return true;
}

void calErase() {
  if (s_prefs.begin(NVS_NAMESPACE, false)) {
    s_prefs.remove(NVS_KEY);
    s_prefs.end();
  }
  s_valid = false;
  loadFallback();
  Serial.println("[cal] calibration erased");
}

// --- mapping ---------------------------------------------------------------

uint8_t applyCal(uint8_t channel, uint16_t raw) {
  const ChannelCal &c = g_cal.ch[channel];

  // Work against endpoints pulled inward by the deadzone, so the last slice of
  // travel at each end reliably saturates rather than hovering near 1 or 126.
  uint16_t m = endMargin(c.lo, c.hi);
  uint16_t lo = c.lo + m;
  uint16_t hi = c.hi - m;

  if (raw <= lo) return 0;
  if (raw >= hi) return 127;

  // A stored mid outside the inset range would divide by zero or invert the
  // curve; fall back to a straight two-point map for that channel.
  uint16_t mid = c.mid;
  if (mid <= lo || mid >= hi) mid = (uint16_t)(((uint32_t)lo + (uint32_t)hi) / 2);

  int32_t out;
  if (raw <= mid) {
    // Lower half of the physical throw -> 0..63
    out = (int32_t)63 * (int32_t)(raw - lo) / (int32_t)(mid - lo);
  } else {
    // Upper half -> 64..127
    out = 64 + (int32_t)63 * (int32_t)(raw - mid) / (int32_t)(hi - mid);
  }

  if (out < 0) out = 0;
  if (out > 127) out = 127;
  return (uint8_t)out;
}

// --- calibration flow ------------------------------------------------------

static void ledBlockingBlink(uint16_t onMs, uint16_t offMs, uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(LED_PIN, LOW);
    delay(offMs);
  }
}

// Blinks at blinkMs while waiting for a press of the BOOT button, then waits
// for the release so one press cannot advance two steps. Entering calibration
// by long-pressing BOOT leaves the button still held, and that is handled for
// free: a held button produces no new press edge, so the first step waits for
// a genuine release-then-press.
static void waitForCalButton(uint16_t blinkMs) {
  buttonsUpdate();
  buttonsClearEdges();

  uint32_t lastToggle = millis();
  uint32_t lastDump = 0;
  bool on = false;
  while (true) {
    buttonsUpdate();
    if (buttonConsumePress(BOOT_BUTTON)) break;
    uint32_t now = millis();
    if (now - lastToggle >= blinkMs) {
      lastToggle = now;
      on = !on;
      digitalWrite(LED_PIN, on ? HIGH : LOW);
    }
    // Live raw values while positioning, so every channel can be confirmed at
    // the right end BEFORE the capture. Without this you are guessing, which
    // is how channels land with a few counts of travel in one half.
    if (now - lastDump >= 400) {
      lastDump = now;
      Serial.print("RAW");
      for (uint8_t i = 0; i < NUM_ANALOG; i++)
        Serial.printf(" %2u:%4u", i + 1, analogReadRaw(i));
      Serial.println();
    }
    delay(2);
  }

  digitalWrite(LED_PIN, LOW);
  while (buttonIsDown(BOOT_BUTTON)) {
    buttonsUpdate();
    delay(2);
  }
  buttonsClearEdges();
}

// Averages CAL_SAMPLES rounds across every channel, ~200ms of settling.
static void capture(uint16_t out[NUM_ANALOG]) {
  digitalWrite(LED_PIN, HIGH);

  uint32_t acc[NUM_ANALOG];
  for (uint8_t i = 0; i < NUM_ANALOG; i++) acc[i] = 0;

  for (uint16_t s = 0; s < CAL_SAMPLES; s++) {
    for (uint8_t i = 0; i < NUM_ANALOG; i++) acc[i] += analogReadRaw(i);
    delay(CAL_SAMPLE_GAP);
  }

  for (uint8_t i = 0; i < NUM_ANALOG; i++) out[i] = (uint16_t)(acc[i] / CAL_SAMPLES);

  digitalWrite(LED_PIN, LOW);
}

void runCalibration() {
  uint16_t lo[NUM_ANALOG], mid[NUM_ANALOG], hi[NUM_ANALOG];

  Serial.println();
  Serial.println("=== CALIBRATION ===");
  Serial.println("Three steps. Press the BOOT button (GPIO 0) after each one.");

  Serial.println("[1/3] Move EVERY fader/pot to its MINIMUM, then press BOOT.");
  waitForCalButton(600);
  capture(lo);

  Serial.println("[2/3] Move EVERY fader/pot to its physical CENTRE, then press BOOT.");
  waitForCalButton(300);
  capture(mid);

  Serial.println("[3/3] Move EVERY fader/pot to its MAXIMUM, then press BOOT.");
  waitForCalButton(120);
  capture(hi);

  uint8_t bad = 0;
  for (uint8_t i = 0; i < NUM_ANALOG; i++) {
    ChannelCal c;
    c.lo = lo[i];
    c.mid = mid[i];
    c.hi = hi[i];

    // Tolerate a channel wired the other way round.
    if (c.lo > c.hi) {
      uint16_t t = c.lo;
      c.lo = c.hi;
      c.hi = t;
      Serial.printf("[cal] ch %u looks reversed; check wiring\n", i + 1);
    }

    bool twoPoint = useTwoPoint(c);

    if (channelSane(c)) {
      g_cal.ch[i] = c;
      uint16_t span = c.hi - c.lo;
      Serial.printf("[cal] ch %2u (GPIO %2u): lo=%4u mid=%4u hi=%4u span=%4u%s%s\n",
                    i + 1, ANALOG_PINS[i], c.lo, c.mid, c.hi, span,
                    span < CAL_LOW_RES_SPAN ? "  LOW RES" : "",
                    twoPoint ? "  2-POINT (taper correction off)" : "");
    } else {
      setLinearDefault(g_cal.ch[i]);
      bad++;
      Serial.printf("[cal] ch %2u (GPIO %2u): BAD (lo=%u mid=%u hi=%u) -> linear default\n",
                    i + 1, ANALOG_PINS[i], lo[i], mid[i], hi[i]);
    }
  }

  bool saved = calSave();
  s_valid = saved && (bad == 0);
  analogResetOutputs();

  if (bad == 0 && saved) {
    Serial.println("[cal] done, all channels good");
    ledBlockingBlink(400, 150, 2);
  } else {
    Serial.printf("[cal] done with %u bad channel(s)\n", bad);
    ledBlockingBlink(80, 80, 8);  // distinct error pattern
  }
  Serial.println();
}
