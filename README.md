# USB MIDI DJ Mixer Controller

Firmware for a DIY DJ mixer control surface built on a **Wemos ESP32-S2 Mini**.
13 pots/faders and 4 buttons, appearing to the host as a class-compliant USB MIDI
device — no drivers, works with Traktor / Serato / Mixxx / Ableton.

## Wiring

| Function | GPIO | Notes |
|---|---|---|
| Analog ch. 1–10 | 1–10 | ADC1 |
| Analog ch. 11–13 | 11–13 | ADC2 — fine, WiFi is never started |
| Button 1–4 | 33, 35, 37, 39 | switch to GND, internal pull-ups, no external resistors |
| BOOT button | 0 | already on the board — drives calibration, sends no MIDI |
| Status LED | 15 | on-board blue LED |
| USB | 19 / 20 | native USB, hardwired on the board |

Pots: wiper to the GPIO, the two ends to 3V3 and GND.

If the board ever resets when you press a button, suspect GPIO 33–37 — they are the
octal-SPI pins, free on the quad-SPI S2 Mini but shared on octal variants.

## MIDI map

Everything on **MIDI channel 1**.

- Pots/faders → **CC 1–13**, in GPIO order
- Buttons → **notes 36, 37, 38, 39**, momentary (Note On vel 127 / Note Off)

All of it lives in `src/config.h` if you want to remap.

## Build & flash

```sh
pio run -t upload
pio device monitor      # calibration prompts and debug output
```

Put the board in bootloader mode first: hold `0`, tap `RST`, release `0`, then:

```sh
pio run -e lolin_s2_mini_manual -t upload
```

That env passes `--before no_reset --after no_reset`. It is needed because esptool's
default reset toggles DTR/RTS, which on the S2's **native USB** kicks the ROM bootloader
straight back out of download mode. The 1200-baud touch does not reboot this firmware
into the bootloader either, so the button press is required for every flash.

## Calibration

The pots can be linear or logarithmic, and the ESP32-S2 ADC is noisy and non-linear
near both rails. A three-point calibration per channel folds all of that into one
correction, so every fader ends up with a linear 0–127 throw.

**It runs automatically the first time**, or whenever nothing valid is stored.

1. LED blinks **slowly** → move every pot/fader to its **minimum**, press **BOOT**
2. LED blinks **medium** → move every pot to its **physical centre**, press **BOOT**
3. LED blinks **fast** → move every pot to its **maximum**, press **BOOT**

Calibration is driven entirely by the board's built-in **BOOT button (GPIO 0)**, so all
four user buttons stay free for MIDI.

While a step is waiting, raw ADC values for all 13 channels are printed to serial four
times a second:

```
RAW  1:  37  2:2026  3:1820  4:  88 ...
```

Confirm every channel really is at the end of its travel before pressing BOOT. This also
identifies which physical fader is wired to which channel — move one and watch which index
changes.

Each step averages 64 readings per channel over ~200 ms. Two slow flashes at the end
means success; eight fast flashes means at least one channel failed its sanity check
(less than 500 counts of travel, or a centre reading outside the ends) — those channels
fall back to a plain linear map and are named on the serial monitor.

Results are stored in NVS with a CRC and survive replugging.

**Short-press BOOT** while running to toggle the same raw dump, with the mapped MIDI value
alongside (`13:1698> 63`). Useful for telling a mis-wired channel from a badly calibrated
one.

**To recalibrate:** long-press **BOOT** for 3 seconds at any time while the controller is
running — no replugging needed. The stored calibration is wiped and the flow restarts from
step 1. Any held notes are released first so the host is not left with a stuck note.

### How the correction works

The centre capture is the pot's *physical* middle, so whatever raw value it happens to
sit at becomes MIDI 63/64 by construction. Below it, `lo → mid` maps onto 0–63; above
it, `mid → hi` maps onto 64–127. For an audio-taper pot that turns a centre reading of
~29 into 63.

Residual non-linearity mid-segment is around 10 LSB on a steep log taper — fine for a
fader. If you ever want it tighter, a gamma fit or a multi-point sweep capture would
be the next step.

## Verifying it works

```sh
system_profiler SPUSBDataType | grep -A5 "MIDI Mixer"   # enumeration
brew install gbevin/tools/receivemidi
receivemidi dev "MIDI Mixer"                            # watch live messages
```

Check that each pot sweeps its CC cleanly 0 → 127, that a pot at physical centre reads
**63 or 64**, that a pot at rest does not dither, and that each button emits the right
note. On macOS the device should also show up in *Audio MIDI Setup → Window → Show MIDI
Studio*.

## Source layout

| File | Purpose |
|---|---|
| `src/config.h` | pin tables, CC/note maps, tuning constants |
| `src/inputs.cpp` | ADC oversampling + EMA filter + send gating, button debounce |
| `src/calibration.cpp` | NVS storage, the capture flow, raw → 7-bit mapping |
| `src/main.cpp` | setup/loop, USB MIDI transmit, LED status, BOOT long-press |

Jitter control: 8× oversampling, an exponential moving average, and a send gate with two
independent accept paths — the pot moved further than the noise floor (`RAW_DEADBAND`,
16 counts), or the value held still for `SETTLE_MS`. The first keeps sweeps responsive,
the second guarantees a fader lands exactly on its endpoint.

The gate deliberately measures movement in **raw ADC counts, not MIDI steps**. A
short-range channel is the reason: one pot here spans only ~430 counts (≈3 counts per MIDI
step) against ~±13 counts of filtered noise, so any output-domain threshold lets that noise
through as real movement. Measured on captured hardware noise, an output-domain gate emitted
60 messages/second from a motionless fader; the raw-domain gate emits none.

### End-of-travel deadzone

The last **5% of travel at each end** (minimum 20 raw counts) saturates to 0 and 127, so a
fader pushed to its stop reliably reads the extreme instead of landing on 1 or 126. The
captured `lo`/`hi` come from a single averaged reading at the mechanical stops, and against
~±13 counts of filtered noise that is otherwise a coin flip — which matters for a channel
fader that must actually close.

Each channel keeps 90% of its throw. The margin is derived from the stored endpoints at
runtime, not saved, so `CAL_END_MARGIN_PCT` can be retuned without recalibrating.

### Short-range channels

A pot that does not swing the full 0–3.3 V still works — `CAL_MIN_SPAN` accepts anything
above 250 counts. Calibration marks these `LOW RES` on serial. They are usable but visibly
stepped: resolution is roughly `span / 128` ADC counts per MIDI step. If a channel reads
`LOW RES` unexpectedly, check its outer legs actually reach 3V3 and GND before accepting it.

## Credits

Firmware written with [Claude Code](https://claude.com/claude-code) (Claude Opus 5), paired
with hardware testing on the actual board — several design choices here came directly from
measured behaviour rather than theory. The raw-count jitter gate and the two-point fallback
both exist because captured ADC data showed the first approach failing.
