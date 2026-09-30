#pragma once
#include <Arduino.h>
#include "config.h"

void inputsBegin();

// --- buttons ---------------------------------------------------------------
// Call buttonsUpdate() often; it debounces and latches edges. The consume*
// helpers return true once per edge and clear it.
void buttonsUpdate();
bool buttonConsumePress(uint8_t i);
bool buttonConsumeRelease(uint8_t i);
bool buttonIsDown(uint8_t i);
void buttonsClearEdges();

// --- analog ----------------------------------------------------------------
// One oversampled read, no filtering. Used by the calibration capture.
uint16_t analogReadRaw(uint8_t ch);

// Runs the filter for every channel. Returns true and fills ch/value when a
// channel has a new 7-bit value worth sending.
bool analogPoll(uint8_t ch, uint8_t *value);

// Current filtered raw value, for debugging.
uint16_t analogFilteredRaw(uint8_t ch);

// Forget the last-sent values so the next poll re-emits everything.
void analogResetOutputs();
