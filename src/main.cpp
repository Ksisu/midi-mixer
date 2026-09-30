// USB MIDI DJ mixer controller - Wemos ESP32-S2 Mini
// 13 pots/faders -> CC 1..13, 4 buttons -> notes 36..39, MIDI channel 1.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>

#include "config.h"
#include "calibration.h"
#include "inputs.h"

Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

static uint32_t s_ledLastMs = 0;
static bool     s_ledOn = false;

// Slow heartbeat in normal operation; the calibration flow drives the LED itself.
static void updateLed() {
  uint32_t now = millis();
  uint16_t period = calIsValid() ? 2000 : 250;  // fast = running on defaults
  if (now - s_ledLastMs >= period) {
    s_ledLastMs = now;
    s_ledOn = !s_ledOn;
    digitalWrite(LED_PIN, s_ledOn ? HIGH : LOW);
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Must happen before usb_midi.begin(), which brings the USB stack up. The
  // lolin_s2_mini variant header hardcodes USB_PRODUCT, so the name can only
  // be overridden at runtime, not with a build flag.
  TinyUSBDevice.setManufacturerDescriptor("ksisu");
  TinyUSBDevice.setProductDescriptor("MIDI Mixer");
  usb_midi.setStringDescriptor("MIDI Mixer");

  // Order matters: on ESP32 the first begin() finalizes the USB descriptor,
  // so every interface has to register before that. Serial after usb_midi
  // means no CDC interface at all, and no serial monitor.
  Serial.begin(115200);

  inputsBegin();

  usb_midi.begin();
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  // Give the host a moment to enumerate so the calibration prompts are not
  // lost, but never block forever on a plain USB charger.
  uint32_t start = millis();
  while (!TinyUSBDevice.mounted() && (millis() - start) < 3000) delay(10);
  delay(300);

  Serial.println();
  Serial.println("MIDI Mixer - ESP32-S2");

  if (!calLoad()) {
    Serial.println("[cal] nothing stored - starting calibration automatically");
    runCalibration();
  }

  analogResetOutputs();
  Serial.println("Running. Long-press BOOT (GPIO 0) for 3s to recalibrate.");
}

// Short-press BOOT to toggle a live dump of raw ADC values. This is the only
// way to tell a mis-wired channel from one that simply was not swept fully
// during calibration, since CC values are post-calibration and clamp.
static bool     s_rawMonitor = false;
static uint32_t s_rawLastMs = 0;

static void rawMonitorTick() {
  if (!s_rawMonitor) return;
  uint32_t now = millis();
  if (now - s_rawLastMs < 250) return;
  s_rawLastMs = now;

  Serial.print("RAW");
  for (uint8_t i = 0; i < NUM_ANALOG; i++) {
    uint16_t r = analogFilteredRaw(i);
    Serial.printf(" %2u:%4u>%3u", i + 1, r, applyCal(i, r));
  }
  Serial.println();
}

// Long-pressing BOOT while running wipes the calibration and starts over.
// Fires once per hold, on reaching the threshold, without waiting for release.
static void checkCalRestart() {
  static uint32_t downSinceMs = 0;
  static bool     fired = false;

  if (!buttonIsDown(BOOT_BUTTON)) {
    // Released. A short press (below the reset threshold) toggles raw output.
    if (buttonConsumeRelease(BOOT_BUTTON) && !fired) {
      s_rawMonitor = !s_rawMonitor;
      Serial.printf("[dbg] raw monitor %s\n", s_rawMonitor ? "ON" : "OFF");
    }
    downSinceMs = 0;
    fired = false;
    return;
  }

  if (downSinceMs == 0) {
    downSinceMs = millis();
    return;
  }
  if (fired || (millis() - downSinceMs) < CAL_RESET_HOLD_MS) return;
  fired = true;

  // Release anything the host thinks is held down before we block for minutes.
  for (uint8_t i = 0; i < NUM_BUTTONS; i++)
    MIDI.sendNoteOff(BUTTON_NOTE[i], 0, MIDI_CHANNEL);

  Serial.println();
  Serial.println("[cal] BOOT held - clearing calibration and restarting it");
  calErase();
  runCalibration();
  analogResetOutputs();
  buttonsClearEdges();
  Serial.println("Running. Long-press BOOT (GPIO 0) for 3s to recalibrate.");
}

void loop() {
  buttonsUpdate();
  checkCalRestart();

  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    if (buttonConsumePress(i))
      MIDI.sendNoteOn(BUTTON_NOTE[i], 127, MIDI_CHANNEL);
    if (buttonConsumeRelease(i))
      MIDI.sendNoteOff(BUTTON_NOTE[i], 0, MIDI_CHANNEL);
  }

  for (uint8_t ch = 0; ch < NUM_ANALOG; ch++) {
    uint8_t value;
    if (analogPoll(ch, &value))
      MIDI.sendControlChange(ANALOG_CC[ch], value, MIDI_CHANNEL);
  }

  // Drain anything the host sends so the USB stack stays healthy.
  MIDI.read();

  rawMonitorTick();

  updateLed();
}
