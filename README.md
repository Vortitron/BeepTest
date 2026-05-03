# BeepTest (ESP32-H2)

“Beep test” style running game for kids using an **ESP32-H2** and a **TM1637 4‑digit 7‑segment display**.

Recommended setup:

- **Control unit**: ESP32 + display + **at least 1 button** + **local buzzer**
- **Dumb far-end remote**: **battery + button + buzzer** (no electronics link to the ESP32)

One end of the room is the **control unit** (ESP32 + display + start/return button + local buzzer).  
The other end is a **dumb remote** (just a **battery + button + buzzer**) with **no connection** to the ESP32.

If the kids “game” it, you can later upgrade to an **optional wired remote** (or wireless) without changing the gameplay rules.

## What it does

1. **Wake**: any button press wakes the device (display shows `----`).
2. **Calibrate**: the next press starts a calibration run; the press after that ends it.
   The Level 1 target time is `calibration - 0.5 s`.
3. **Run rounds**: each round is **4 runs**. The display shows **progress dots** so each
   button press has an *instant* visual response, regardless of what the buzzers are doing:
   - `----` (none done) -> `o---` -> `oo--` -> `ooo-` -> `oooo` (round complete)
4. **LED stages during a run** (so you can glance at it from across the room):
   - **Green**: 0 % to 50 % of the target time.
   - **Yellow**: half-way passed.
   - **Orange**: in the last 2 s before the target.
   - **Red (flashing)**: overtime, plus the buzzer goes solid.
5. **Countdown hint**: when there are <= 5 s remaining, the display briefly alternates
   between the progress dots and the seconds remaining.
6. **Stats overlay**: 1 s after each press the display briefly shows the lap time, the
   target, and the difference (signed seconds), then returns to the progress dots.
7. **Level up**: after 4 runs we drop the fastest and slowest, then average the middle 2.
   - If the average is faster than the target: **level up** and reduce the target by 0.5 s.
   - Otherwise: stay on the same level.
8. **Anti-cheat / realism**: laps faster than `MIN_LAP_MS` (default 3 s) are ignored.
9. **Sleep**: the device auto-blanks the display and turns the LED off after 3 minutes
   of inactivity. The `[raw]` serial trace is suppressed in this state so the on-board
   UART activity LED does not blink while "asleep".
10. **Hold start ~2 s**: pause (resets the current lap). Hold again from pause: full
    session reset.

## Modes (battery remote now, wired remote later)

The firmware supports two setups via a build-time flag in `firmware/BeepTest/BeepTest.ino`:

- **One-side mode (default)**: `USE_WIRED_REMOTE = 0`
  - Far end is truly dumb: battery + button + buzzer (no signal back).
  - A lap is recorded when you press a button on the control unit when the kid returns (start button and/or an optional 2nd button).
  - **Anti-cheat** is done via `MIN_LAP_MS` (minimum lap time).
- **Wired remote mode (optional)**: set `USE_WIRED_REMOTE = 1`
  - Remote button and (optional) remote buzzer are connected back to the ESP32.
  - A lap is recorded only after: **remote button** → **start/return button**.
  - Beeps can play on both buzzers (local + remote).

## Firmware

The Arduino sketch lives here:

- `firmware/BeepTest/BeepTest.ino`

### Requirements

- Arduino IDE (or PlatformIO)
- ESP32 board support package (ESP32-H2)
- Library: `TM1637Display`

## Wiring / Pinout

The pin map is defined at the top of `firmware/BeepTest/BeepTest.ino`.  
**Change these pins to match your wiring.**

### Default pins (as in the sketch)

In **one-side mode** you only need: **TM1637 CLK/DIO**, **BTN_START**, and **BUZZER_LOCAL**.  
`BTN_REMOTE` and `BUZZER_REMOTE` can be left unconnected unless you want a 2nd local button or a wired remote later.

| Signal | Description | ESP32-H2 GPIO |
|---|---|---:|
| TM1637 CLK | 7-seg clock | 12 |
| TM1637 DIO | 7-seg data  | 11 |
| BTN_START | control-side start/return (INPUT_PULLUP) | 10 |
| BTN_REMOTE | **optional** 2nd local button (one-side mode) **or** wired remote button (wired mode) | 9 |
| BUZZER_LOCAL | control-side buzzer (active buzzer) | 14 |
| BUZZER_REMOTE | **optional** wired remote buzzer output (wired mode) | 13 |

### Button wiring

- Buttons are configured as `INPUT_PULLUP`.
- Wire each button between its GPIO pin and **GND** (pressed = LOW).

### Buzzer wiring

- The sketch assumes **active buzzers** (they beep when you drive the pin HIGH).
- Wire buzzer “+” to the GPIO, buzzer “–” to GND.
- If you have **passive** piezo buzzers, you’ll likely want to replace the on/off drive with `tone()`/PWM.

### Dumb battery remote (no wires)

This is *not* connected to the ESP32. It’s simply:

- Battery → button → active buzzer

When pressed, it beeps locally so the kid knows they hit the far end.

### If you later wire the remote (optional)

If you convert the far end to a wired remote:

- Run **GND + signal** as a pair (shared ground is required).
- Consider twisted pair / shielded cable if you get false triggers.
- You can increase the debounce time (`DEBOUNCE_MS`) if needed.

## Tuning / Settings (most useful constants)

These are all at the top of `firmware/BeepTest/BeepTest.ino`:

- `USE_WIRED_REMOTE`: 0 = battery remote / one-side mode (default), 1 = wired remote
- `RUNS_PER_LEVEL`: runs per round (default **4**; the progress-dot display assumes <= 4)
- `MIN_LAP_MS`: minimum time between lap hits (default **3000 ms**)
- `MIN_TARGET_MS`: clamp on the per-run target so it never gets absurdly short (default **3000 ms**)
- `TARGET_BONUS_MS`: how much faster the next target is vs the calibration / round average (default **500 ms**)
- `NEAR_TIME_MS`: orange-LED threshold before the target (default **2000 ms**)
- `COUNTDOWN_HINT_MS`: when remaining drops below this, the display alternates with the seconds countdown (default **5000 ms**)
- `OVERTIME_FLASH_MS`: flash period for the display + LED while overtime (default **250 ms**)
- `LAP_CONFIRM_MS`: tiny "tick" on the local buzzer when a lap registers; set to **0** to mute (default **60 ms**)
- `OFF_AFTER_MS`: inactivity timeout before the device blanks and stops driving the LED (default **180000 ms**)
- `RESET_HOLD_MS`: hold duration on the start button to pause / reset (default **2000 ms**)
- `IDLE_RAW_LOG_MS`: how often the `[raw]` pin trace is repeated when nothing changes (default **5000 ms**); raw logging is fully suppressed in the OFF state, which is why the on-board UART LED no longer blinks while the device is asleep.

## How to use

1. Flash the sketch to the ESP32-H2.
2. Press a button to **wake** the device (display shows `----`, LED dim blue).
3. Press again to start the **calibration** run; press again at the far end (or as soon
   as you want to define your starting pace) to end it. The Level 1 target = calibration
   time minus 0.5 s (clamped to `MIN_TARGET_MS`).
4. The display scrolls `LEVEL 1` (a press skips the scroll), then the round begins.
5. For each lap:
   - One-side mode: hit the dumb battery-powered far-end button (it beeps locally so the
     runner knows they reached it), run back, and press a button on the control unit.
     The progress dots advance immediately on each registered press.
   - Wired remote mode: remote button (far side) -> start/return button (control side).
6. After 4 runs, the device evaluates the round, beeps, scrolls the new (or same) level,
   and starts the next round.
7. Hold the start button for ~2 s to pause; hold again from pause to reset the session.

## Next ideas (optional)

- Make the far end truly wireless (second ESP / ESP-NOW)
- Show best lap / last lap time instead of only lap number
- Add a “strict” mode that requires the beep timing (not just “<= baseline”)