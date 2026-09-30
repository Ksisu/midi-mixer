#pragma once
#include <Arduino.h>
#include "config.h"

struct ChannelCal {
  uint16_t lo;   // raw at the physical minimum
  uint16_t mid;  // raw at the physical centre
  uint16_t hi;   // raw at the physical maximum
};

#define CAL_MAGIC   0x4D584301UL
#define CAL_VERSION 1

struct CalData {
  uint32_t   magic;
  uint8_t    version;
  uint8_t    pad[3];
  ChannelCal ch[NUM_ANALOG];
  uint32_t   crc;      // over every byte before this field
};
// Not packed: taking references into the fields needs natural alignment, and
// the layout only has to be stable for this one compiler/target. CAL_VERSION
// guards against it ever changing.

extern CalData g_cal;

// True once a valid calibration is in g_cal (loaded or freshly captured).
bool calIsValid();

// Loads from NVS. Returns false when nothing valid is stored, in which case
// g_cal holds the linear fallback.
bool calLoad();

bool calSave();
void calErase();

// Raw ADC reading -> 0..127, with the log-taper / ADC non-linearity corrected.
uint8_t applyCal(uint8_t channel, uint16_t raw);

// Blocking three-step capture driven by the BOOT button. Saves on completion.
void runCalibration();
