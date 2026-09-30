#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Hardware map - Wemos ESP32-S2 Mini
// ---------------------------------------------------------------------------

#define NUM_ANALOG  13
#define NUM_BUTTONS 4

// GPIO 1..10 are on ADC1, GPIO 11..13 on ADC2. ADC2 is usable here because we
// never bring up WiFi.
static const uint8_t ANALOG_PINS[NUM_ANALOG] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

// Switches to GND, internal pull-ups, active LOW. On the ESP32-S2 (unlike the
// classic ESP32) these are full GPIOs with working pull-ups.
static const uint8_t BUTTON_PINS[NUM_BUTTONS] = {33, 35, 37, 39};

// On-board blue LED of the S2 Mini.
#define LED_PIN 15

// The board's built-in BOOT button. It drives the whole calibration flow, so
// all four user buttons stay free for MIDI. Tracked alongside them internally
// as one extra entry, but never sends MIDI.
#define BOOT_BUTTON_PIN 0
#define NUM_ALL_BUTTONS (NUM_BUTTONS + 1)
#define BOOT_BUTTON     NUM_BUTTONS

// ---------------------------------------------------------------------------
// MIDI map - all numbers live here so remapping is a one-line edit
// ---------------------------------------------------------------------------

#define MIDI_CHANNEL 1

static const uint8_t ANALOG_CC[NUM_ANALOG] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

static const uint8_t BUTTON_NOTE[NUM_BUTTONS] = {36, 37, 38, 39};

// ---------------------------------------------------------------------------
// ADC / filtering tuning
// ---------------------------------------------------------------------------

#define ADC_BITS        12     // 0..4095
#define ADC_MAX         4095
#define ADC_OVERSAMPLE  8      // reads averaged per scan pass
#define EMA_SHIFT       5      // exponential moving average strength

// Movement gate, in RAW counts rather than MIDI steps. It has to be noise-
// referenced: a narrow channel (ch5 spans only ~430 counts, ~3 per MIDI step)
// sees ~±11 counts of filtered noise, which is several MIDI steps, so any
// output-domain threshold lets that noise straight through.
#define RAW_DEADBAND    16

// Below the deadband a new value is still accepted once it has held still this
// long, so a fader always lands on its exact final value.
#define SETTLE_MS       30
// Per-channel floor between two CC messages, so a fast swipe cannot flood USB.
#define MIN_SEND_MS     2

#define BUTTON_DEBOUNCE_MS 5

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

#define CAL_SAMPLES      64    // averaged per channel per capture step
#define CAL_SAMPLE_GAP   3     // ms between capture rounds (~200ms total)
// Lowered to accept genuinely short-range pots (ch5 spans ~430 counts). Such a
// channel works, just coarsely - RAW_DEADBAND is what keeps it from chattering.
#define CAL_MIN_SPAN     250   // minimum raw hi-lo for a channel to be trusted
// Warn on serial below this: usable, but visibly stepped.
#define CAL_LOW_RES_SPAN 1200
// Each HALF of the throw needs room too. A total span alone is not enough: a
// channel with mid=1698 hi=1703 spans 1669 counts yet maps MIDI 64..127 onto
// 5 ADC counts, so one count of noise swings the output ~12 steps and the
// channel screams. Both segments must be wide enough to be worth sending.
// Not a reject threshold: a channel whose half-throw is narrower than this
// simply loses its taper correction and gets mapped end-to-end instead. Forcing
// the physical centre to MIDI 63 only helps when that half has counts to spare;
// below this it would spend 64 MIDI steps on a few counts of noise.
#define CAL_MIN_SEGMENT  100
// Hold BOOT at any time while running to wipe the calibration and start over.
#define CAL_RESET_HOLD_MS 3000

// End-of-travel deadzone. The captured lo/hi come from one averaged reading at
// the mechanical stops, so against ~±13 counts of filtered noise a fader held
// hard against its end lands on 1 or 126 as often as 0 or 127. Insetting the
// mapped range makes the last slice of travel saturate instead. The floor keeps
// the deadzone above the noise floor on short-range channels.
#define CAL_END_MARGIN_PCT 5    // of span, at each end
#define CAL_END_MARGIN_MIN 20   // raw counts
