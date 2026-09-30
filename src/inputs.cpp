#include "inputs.h"
#include "calibration.h"

// --- buttons ---------------------------------------------------------------

struct Button {
  bool     stable;        // debounced logical state, true = pressed
  bool     lastRead;
  uint32_t lastChangeMs;
  bool     pressEdge;
  bool     releaseEdge;
};

static Button s_btn[NUM_ALL_BUTTONS];

// User buttons first, the built-in BOOT button last at index BOOT_BUTTON.
static uint8_t buttonPin(uint8_t i) {
  return (i < NUM_BUTTONS) ? BUTTON_PINS[i] : BOOT_BUTTON_PIN;
}

// --- analog ----------------------------------------------------------------

static int32_t  s_ema[NUM_ANALOG];            // value << EMA_SHIFT
static bool     s_primed[NUM_ANALOG];
static uint8_t  s_lastSent[NUM_ANALOG];
static uint16_t s_lastSentRaw[NUM_ANALOG];
static uint8_t  s_candidate[NUM_ANALOG];
static uint32_t s_candidateSinceMs[NUM_ANALOG];
static uint32_t s_lastSendMs[NUM_ANALOG];
static bool     s_everSent[NUM_ANALOG];

void inputsBegin() {
  analogReadResolution(ADC_BITS);
  analogSetAttenuation(ADC_11db);  // full 0..3.3V swing

  for (uint8_t i = 0; i < NUM_ANALOG; i++) {
    pinMode(ANALOG_PINS[i], INPUT);
    s_primed[i] = false;
    s_everSent[i] = false;
    s_lastSendMs[i] = 0;
  }

  for (uint8_t i = 0; i < NUM_ALL_BUTTONS; i++) {
    pinMode(buttonPin(i), INPUT_PULLUP);
    s_btn[i].stable = false;
    s_btn[i].lastRead = false;
    s_btn[i].lastChangeMs = millis();
    s_btn[i].pressEdge = false;
    s_btn[i].releaseEdge = false;
  }
}

void buttonsUpdate() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_ALL_BUTTONS; i++) {
    bool raw = (digitalRead(buttonPin(i)) == LOW);  // active LOW
    if (raw != s_btn[i].lastRead) {
      s_btn[i].lastRead = raw;
      s_btn[i].lastChangeMs = now;
    } else if (raw != s_btn[i].stable &&
               (now - s_btn[i].lastChangeMs) >= BUTTON_DEBOUNCE_MS) {
      s_btn[i].stable = raw;
      if (raw) s_btn[i].pressEdge = true;
      else     s_btn[i].releaseEdge = true;
    }
  }
}

bool buttonConsumePress(uint8_t i) {
  if (!s_btn[i].pressEdge) return false;
  s_btn[i].pressEdge = false;
  return true;
}

bool buttonConsumeRelease(uint8_t i) {
  if (!s_btn[i].releaseEdge) return false;
  s_btn[i].releaseEdge = false;
  return true;
}

bool buttonIsDown(uint8_t i) { return s_btn[i].stable; }

void buttonsClearEdges() {
  for (uint8_t i = 0; i < NUM_ALL_BUTTONS; i++) {
    s_btn[i].pressEdge = false;
    s_btn[i].releaseEdge = false;
  }
}

uint16_t analogReadRaw(uint8_t ch) {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < ADC_OVERSAMPLE; i++) acc += analogRead(ANALOG_PINS[ch]);
  return (uint16_t)(acc / ADC_OVERSAMPLE);
}

uint16_t analogFilteredRaw(uint8_t ch) {
  return (uint16_t)(s_ema[ch] >> EMA_SHIFT);
}

void analogResetOutputs() {
  for (uint8_t i = 0; i < NUM_ANALOG; i++) s_everSent[i] = false;
}

bool analogPoll(uint8_t ch, uint8_t *value) {
  uint16_t sample = analogReadRaw(ch);

  if (!s_primed[ch]) {
    s_ema[ch] = (int32_t)sample << EMA_SHIFT;
    s_primed[ch] = true;
  } else {
    s_ema[ch] += (int32_t)sample - (s_ema[ch] >> EMA_SHIFT);
  }

  uint8_t  v   = applyCal(ch, analogFilteredRaw(ch));
  uint32_t now = millis();

  if (v != s_candidate[ch]) {
    s_candidate[ch] = v;
    s_candidateSinceMs[ch] = now;
  }

  // First value after boot/calibration is always emitted so the host learns
  // where every fader actually sits.
  uint16_t raw = analogFilteredRaw(ch);

  if (!s_everSent[ch]) {
    if ((now - s_candidateSinceMs[ch]) < SETTLE_MS) return false;
    s_everSent[ch] = true;
    s_lastSent[ch] = v;
    s_lastSentRaw[ch] = raw;
    s_lastSendMs[ch] = now;
    *value = v;
    return true;
  }

  if (v == s_lastSent[ch]) return false;
  if ((now - s_lastSendMs[ch]) < MIN_SEND_MS) return false;

  int32_t moved = (int32_t)raw - (int32_t)s_lastSentRaw[ch];
  if (moved < 0) moved = -moved;

  // Two independent ways to accept a change:
  //   - the pot physically moved further than the noise floor, or
  //   - the value has held still long enough to be real.
  // The first keeps a sweep responsive, the second guarantees the fader lands
  // exactly on its endpoint. Judging "real movement" in raw counts is what
  // makes this work on a short-range channel as well as a full-range one.
  bool realMove = (moved >= RAW_DEADBAND);
  bool settled  = ((now - s_candidateSinceMs[ch]) >= SETTLE_MS);
  if (!realMove && !settled) return false;

  s_lastSent[ch] = v;
  s_lastSentRaw[ch] = raw;
  s_lastSendMs[ch] = now;
  *value = v;
  return true;
}
