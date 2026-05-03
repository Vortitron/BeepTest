// BeepTest: running game helper for ESP32-H2 + TM1637
// - One press to wake
// - One press to "go" (start calibration run)
// - Next press ends calibration run; Level 1 target = calibration - 0.5s
// - Then 4-run rounds: display shows progress dots ("o---", "oo--", "ooo-", "oooo")
//   so each press has an *instant* visual response, independent of the buzzers.
//   Under 5s remaining, the display briefly alternates with the seconds countdown.
// - LED stages while running: green -> yellow at halfway -> orange in the last
//   2s -> flashing red in overtime.
// - After 4 runs: drop min/max, average middle 2
//   - if avg < target: level up, new target = avg - 0.5s
//   - else: stay on same level
// - Hold start ~2s: pause (resets current lap). Hold again from pause: full reset.
// Buttons use INPUT_PULLUP (wire to GND). Buzzers assumed "active".

#include <Arduino.h>
#include <TM1637Display.h>

#include "BeepTypes.h"

// ---------------- Serial debug ----------------
#ifndef ENABLE_SERIAL_DEBUG
#define ENABLE_SERIAL_DEBUG 1
#endif

constexpr uint32_t SERIAL_BAUD = 115200;

#if ENABLE_SERIAL_DEBUG
#define DBG_BEGIN() Serial.begin(SERIAL_BAUD)
#define DBG_PRINTLN(x) Serial.println(x)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_BEGIN() ((void)0)
#define DBG_PRINTLN(x) ((void)0)
#define DBG_PRINTF(...) ((void)0)
#endif

// ---------------- Build-time options ----------------
// If you have a *wired* remote unit (remote button + remote buzzer connected back to the ESP32),
// set this to 1. For a truly "dumb" battery remote (no wires back), leave it 0.
#define USE_WIRED_REMOTE 0

// Many TM1637 modules do not have per-digit dots wired (only the middle ':').
// Use a heartbeat animation on the ':' instead of brightness pulsing.
#ifndef USE_BRIGHTNESS_PULSE
#define USE_BRIGHTNESS_PULSE 0
#endif

// ---------------- Pin map (adjust to your wiring) ----------------
// NOTE: Your ESP32-H2 dev board pinout doesn't expose GPIO7. These defaults target boards
// that break out GP9..GP14 (as in your pinout image). Change as needed.
constexpr uint8_t PIN_TM1637_CLK = 12;
constexpr uint8_t PIN_TM1637_DIO = 11;

constexpr uint8_t PIN_BTN_START   = 10; // start / return / hold-to-reset (control side)
constexpr uint8_t PIN_BTN_REMOTE  = 9;  // optional 2nd local button (one-side) OR wired remote button

constexpr uint8_t PIN_BUZZER_LOCAL  = 14;  // control-side buzzer
constexpr uint8_t PIN_BUZZER_REMOTE = 13;  // far-side buzzer (wired remote)

// ---------------- Tunables ----------------
constexpr uint32_t COUNTDOWN_MS      = 3000;
constexpr uint32_t OFF_AFTER_MS      = 180000;  // auto-off after inactivity
constexpr uint32_t RESET_HOLD_MS     = 2000;

constexpr uint32_t MIN_LAP_MS        = 3000;     // minimum time between lap hits
// Some active buzzers need >100ms before they audibly start.
constexpr uint16_t BEEP_MS           = 250;
constexpr uint16_t BEEP_GAP_MS       = 120;

constexpr uint32_t LEVEL_UP_BEEP_MS  = 900;
constexpr uint32_t MIN_TARGET_MS     = 3000;     // clamp target so it never gets absurdly short
constexpr uint8_t  RUNS_PER_LEVEL    = 4;        // runs per round; progress dots assume <= 4
constexpr uint32_t TARGET_BONUS_MS   = 500;     // subtract this from (calibration/avg) when setting target
constexpr uint16_t OVERTIME_FLASH_MS = 250;      // flash period when overtime (ms)
constexpr uint32_t NEAR_TIME_MS      = 2000;     // "nearly out of time" threshold for LED (ms)
constexpr uint32_t COUNTDOWN_HINT_MS = 5000;     // under 5s remaining, briefly show countdown
constexpr uint16_t LAP_CONFIRM_MS    = 60;       // tiny local-buzzer "tick" on a registered lap (0 = off)

constexpr uint32_t STATS_DELAY_MS    = 1000;     // after a press, wait 1s then show stats
constexpr uint32_t STATS_STEP_MS     = 500;      // each stats screen duration

// Idle-state serial logging: only print "[raw]" pin levels when something
// actually changes, OR every IDLE_RAW_LOG_MS as a heartbeat.
// This stops the on-board UART TX LED from blinking while the device is asleep.
constexpr uint32_t IDLE_RAW_LOG_MS   = 5000;

// Heartbeat pattern for the middle ':' (so it doesn't look like a normal clock blink)
// Period 1200ms: ON 70ms, OFF 110ms, ON 140ms, OFF rest.
constexpr uint16_t COLON_HB_PERIOD_MS = 1200;
constexpr uint16_t COLON_HB_ON1_MS    = 70;
constexpr uint16_t COLON_HB_GAP_MS    = 110;
constexpr uint16_t COLON_HB_ON2_MS    = 140;

// ---------------- Optional RGB LED support ----------------
#ifndef USE_RGB_LED
#define USE_RGB_LED 1
#endif

#if USE_RGB_LED && defined(ARDUINO_ARCH_ESP32)
#include <esp32-hal-rgb-led.h> // provides neopixelWrite(pin, r, g, b)
#endif

// Resolve the onboard RGB LED pin from the board variant if available.
// (Avoid naming it PIN_RGB_LED because some variants already define that macro.)
#if USE_RGB_LED
  #if defined(RGB_BUILTIN)
    constexpr uint8_t RGB_LED_PIN = RGB_BUILTIN;
  #elif defined(PIN_RGB_LED)
    constexpr uint8_t RGB_LED_PIN = PIN_RGB_LED;
  #else
    constexpr uint8_t RGB_LED_PIN = 8; // fallback
  #endif
#endif

// ---------------- Display helpers ----------------
TM1637Display display(PIN_TM1637_CLK, PIN_TM1637_DIO);

// Character segments for a few letters
constexpr uint8_t CHAR_L = SEG_D | SEG_E | SEG_F;
constexpr uint8_t CHAR_E = SEG_A | SEG_D | SEG_E | SEG_F | SEG_G;
constexpr uint8_t CHAR_V = SEG_C | SEG_D | SEG_E; // approximation
constexpr uint8_t CHAR_P = SEG_A | SEG_B | SEG_E | SEG_F | SEG_G;
constexpr uint8_t CHAR_A = SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
constexpr uint8_t CHAR_U = SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
constexpr uint8_t CHAR_S = SEG_A | SEG_C | SEG_D | SEG_F | SEG_G;
constexpr uint8_t CHAR_DASH = SEG_G;
// Lowercase 'o' (small loop in the bottom half) - used for the round progress dots.
constexpr uint8_t CHAR_o = SEG_C | SEG_D | SEG_E | SEG_G;
// TM1637Display uses the MSB (0x80) of each digit for the decimal point / colon dots.
constexpr uint8_t DOT_BIT = 0x80;

// TM1637 brightness is typically 0..7. We update only when it changes.
uint8_t lastBrightness = 255;
void updateBrightnessPulse(bool enabled) {
#if USE_BRIGHTNESS_PULSE
  uint8_t target = 4;
  if (enabled) {
    // Triangle wave over 2 seconds: 1s up, 1s down
    uint32_t t = millis() % 2000;
    uint8_t v = (t < 1000) ? (uint8_t)(t / 125) : (uint8_t)((2000 - t) / 125); // 0..8
    if (v > 7) v = 7;
    target = (uint8_t)min<uint8_t>(7, (uint8_t)(1 + v)); // 1..7
  }
  if (target != lastBrightness) {
    display.setBrightness(target, true);
    lastBrightness = target;
  }
#else
  (void)enabled;
#endif
}

bool colonHeartbeatOn(uint32_t now) {
  uint16_t t = (uint16_t)(now % COLON_HB_PERIOD_MS);
  if (t < COLON_HB_ON1_MS) return true;
  if (t < (uint16_t)(COLON_HB_ON1_MS + COLON_HB_GAP_MS)) return false;
  if (t < (uint16_t)(COLON_HB_ON1_MS + COLON_HB_GAP_MS + COLON_HB_ON2_MS)) return true;
  return false;
}

// ---------------- Button debounce ----------------
constexpr uint16_t DEBOUNCE_MS = 25;

void updateButton(DebouncedButton &b, uint32_t now) {
  bool reading = digitalRead(b.pin) == LOW;
  b.fell = false;
  b.rose = false;
  if (reading != b.lastReading) {
    b.changedAt = now;
    b.lastReading = reading;
  }
  if ((now - b.changedAt) > DEBOUNCE_MS && reading != b.stable) {
    b.stable = reading;
    if (b.stable) {
      b.fell = true;
    } else {
      b.rose = true;
    }
  }
}

// ---------------- Buzzer helpers ----------------
void shortBeep(const Buzzer &bz, uint16_t durMs = BEEP_MS) {
  digitalWrite(bz.pin, HIGH);
  delay(durMs);
  digitalWrite(bz.pin, LOW);
}

void doubleBeep(const Buzzer &bz, uint16_t gapMs = BEEP_GAP_MS) {
  shortBeep(bz);
  delay(gapMs);
  shortBeep(bz);
}

void bothShort(const Buzzer &a, const Buzzer &b) {
  shortBeep(a);
  shortBeep(b);
}

void bothDouble(const Buzzer &a, const Buzzer &b) {
  doubleBeep(a);
  doubleBeep(b);
}

void setBuzzContinuous(bool on) {
#if USE_WIRED_REMOTE
  digitalWrite(PIN_BUZZER_LOCAL, on ? HIGH : LOW);
  digitalWrite(PIN_BUZZER_REMOTE, on ? HIGH : LOW);
#else
  digitalWrite(PIN_BUZZER_LOCAL, on ? HIGH : LOW);
#endif
}

// Cache last colour so we don't re-bit-bang the WS2812 every loop with the same value.
// (Helps avoid any chance of visible "twitching" of the LED when nothing has changed,
// and reduces wasted CPU/IO when sleeping.)
uint8_t lastLedR = 0, lastLedG = 0, lastLedB = 0;
bool    lastLedValid = false;

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (lastLedValid && r == lastLedR && g == lastLedG && b == lastLedB) return;
  lastLedR = r; lastLedG = g; lastLedB = b;
  lastLedValid = true;
#if USE_RGB_LED && defined(ARDUINO_ARCH_ESP32)
  neopixelWrite(RGB_LED_PIN, r, g, b);
#else
  (void)r; (void)g; (void)b;
#endif
}

// LED colour stages while a run is active:
//   green  : 0%   .. 50%  of target              -> "you've got time"
//   yellow : 50%  .. (target - NEAR_TIME_MS)     -> "halfway through"
//   orange : last NEAR_TIME_MS before target     -> "running out"
//   red    : overtime (active flash for urgency)
void updateRunLed(uint32_t elapsedMs, uint32_t targetMsLocal, uint32_t now) {
  if (targetMsLocal == 0) {
    setRgb(0, 0, 0);
    return;
  }
  if (elapsedMs >= targetMsLocal) {
    bool flashOn = ((now / OVERTIME_FLASH_MS) % 2) == 0;
    setRgb(flashOn ? 255 : 30, 0, 0);
    return;
  }
  uint32_t remaining = targetMsLocal - elapsedMs;
  if (remaining <= NEAR_TIME_MS) {
    setRgb(255, 100, 0); // orange
  } else if (elapsedMs * 2 >= targetMsLocal) {
    setRgb(180, 160, 0); // yellow (halfway passed)
  } else {
    setRgb(0, 200, 0);   // green
  }
}

// Slow blue "breathing" used for PAUSED so it's clearly different from a hung/dead state.
void updatePausedLed(uint32_t now) {
  // 2s period triangle 0..40..0
  uint32_t t = now % 2000;
  uint8_t b = (t < 1000) ? (uint8_t)(t * 40 / 1000) : (uint8_t)((2000 - t) * 40 / 1000);
  setRgb(0, 0, b);
}

// ---------------- State ----------------
State state = State::OFF;
State lastLoggedState = State::OFF;
uint8_t level = 1;
uint8_t runIndex = 0;                  // 0..(RUNS_PER_LEVEL-1)
uint32_t targetMs = 0;                 // current per-run target
uint32_t runTimesMs[RUNS_PER_LEVEL] = {0};
uint32_t calibStartedAt = 0;
uint32_t runStartedAt = 0;
uint32_t lastActivity = 0;
bool inOvertime = false;
uint32_t startHoldBegan = 0;
bool startHoldHandled = false;

// Stats overlay bookkeeping (briefly shows lap/target/diff after each press)
uint16_t totalLaps = 0;                // total laps across the whole session
uint32_t lastLapMs = 0;
int32_t  lastDiffMs = 0;               // lap - target (positive = slower)
uint32_t statsPendingAt = 0;           // 0 if none, else time to start stats overlay
uint32_t statsStepUntil = 0;
uint8_t  statsPhase = 0;               // 0=lap, 1=target, 2=diff

DebouncedButton btnStart{PIN_BTN_START};
DebouncedButton btnRemote{PIN_BTN_REMOTE};
Buzzer buzLocal{PIN_BUZZER_LOCAL};
Buzzer buzRemote{PIN_BUZZER_REMOTE};

const char* stateName(State s) {
  switch (s) {
    case State::OFF: return "OFF";
    case State::READY: return "READY";
    case State::CALIB_RUNNING: return "CALIB_RUNNING";
    case State::SCROLL_LEVEL: return "SCROLL_LEVEL";
    case State::RUN_ACTIVE: return "RUN_ACTIVE";
    case State::PAUSED: return "PAUSED";
    case State::SHOW_SUMMARY: return "SHOW_SUMMARY";
  }
  return "?";
}

void logStateIfChanged() {
  if (state != lastLoggedState) {
    DBG_PRINTF("[state] %s -> %s\n", stateName(lastLoggedState), stateName(state));
    lastLoggedState = state;
  }
}

// ---------------- Display functions ----------------
void showDashes() {
  uint8_t segs[4] = {CHAR_DASH, CHAR_DASH, CHAR_DASH, CHAR_DASH};
  display.setSegments(segs);
}

void showBlank() {
  uint8_t segs[4] = {0, 0, 0, 0};
  display.setSegments(segs);
}

void showTime(uint32_t elapsedMs) {
  uint32_t totalSec = elapsedMs / 1000;
  uint8_t mins = (totalSec / 60) % 100;
  uint8_t secs = totalSec % 60;
  uint16_t packed = mins * 100 + secs;
  display.showNumberDecEx(packed, 0b01000000, true); // colon on
}

void showCountdownOnly(uint32_t sec, bool colonOn) {
  if (sec > 99) sec = 99;
  uint8_t tens = (uint8_t)(sec / 10);
  uint8_t ones = (uint8_t)(sec % 10);
  uint8_t segs[4] = {0, 0, display.encodeDigit(tens), display.encodeDigit(ones)};
  if (colonOn) segs[1] |= DOT_BIT;
  display.setSegments(segs);
}

// Round-progress dots: e.g. for 4 runs per round and 2 done -> "oo--".
// Gives the player an instant visual on every button press, independent of buzzers.
void showProgress(uint8_t done, uint8_t total) {
  uint8_t maxShow = total;
  if (maxShow > 4) maxShow = 4;
  if (done > maxShow) done = maxShow;
  uint8_t segs[4] = {0, 0, 0, 0};
  for (uint8_t i = 0; i < maxShow; i++) {
    segs[i] = (i < done) ? CHAR_o : CHAR_DASH;
  }
  display.setSegments(segs);
}

void showDiffSeconds(int32_t diffMs) {
  // diff in whole seconds with sign in first digit (dash for negative/faster)
  int32_t sec = diffMs / 1000;
  bool neg = sec < 0;
  uint32_t mag = (uint32_t)(neg ? -sec : sec);
  if (mag > 999) mag = 999;
  uint8_t hundreds = (uint8_t)((mag / 100) % 10);
  uint8_t tens = (uint8_t)((mag / 10) % 10);
  uint8_t ones = (uint8_t)(mag % 10);
  uint8_t segs[4] = {
    neg ? CHAR_DASH : 0,
    display.encodeDigit(hundreds),
    display.encodeDigit(tens),
    display.encodeDigit(ones),
  };
  display.setSegments(segs);
}

void showPaused() {
  uint8_t segs[4] = {CHAR_P, CHAR_A, CHAR_U, CHAR_S};
  display.setSegments(segs);
}

// ---- simple non-blocking scroll "LEVEL n" (approximated) ----
char scrollBuf[32];
uint8_t scrollLen = 0;
uint8_t scrollPos = 0;
uint32_t scrollNextAt = 0;

uint8_t charToSeg(char c) {
  if (c >= '0' && c <= '9') return display.encodeDigit(c - '0');
  switch (c) {
    case ' ': return 0;
    case '-': return CHAR_DASH;
    case 'L': return CHAR_L;
    case 'E': return CHAR_E;
    case 'V': return CHAR_V;
    default: return 0;
  }
}

void startScrollLevel(uint8_t lvl) {
  // Build: "    LEVEL1    " (7-seg approximations)
  // Pad with spaces so it scrolls nicely across 4 digits.
  snprintf(scrollBuf, sizeof(scrollBuf), "    LEVEL%u    ", (unsigned)(lvl % 10));
  scrollLen = (uint8_t)strlen(scrollBuf);
  scrollPos = 0;
  scrollNextAt = millis();
}

bool updateScroll(uint32_t now) {
  if (scrollLen < 4) return true;
  if (now < scrollNextAt) return false;
  scrollNextAt = now + 200;

  uint8_t segs[4] = {
    charToSeg(scrollBuf[scrollPos + 0]),
    charToSeg(scrollBuf[scrollPos + 1]),
    charToSeg(scrollBuf[scrollPos + 2]),
    charToSeg(scrollBuf[scrollPos + 3]),
  };
  display.setSegments(segs);

  if (scrollPos + 4 >= scrollLen) return true;
  scrollPos++;
  return false;
}

void startupSelfTest() {
  DBG_PRINTLN("[boot] Self-test start");

  // Display: force all segments on (more robust than relying on digits)
  {
    uint8_t segs[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    display.setSegments(segs);
  }
  delay(400);
  // Display: count 3..0 quickly to confirm digits update
  for (int i = 3; i >= 0; i--) {
    display.showNumberDec(i, false);
    delay(150);
  }
  showDashes();

  // Buzzers: local always; remote only if wired mode
  DBG_PRINTLN("[boot] Buzzer test (local)");
  shortBeep(buzLocal, 500);
  delay(BEEP_GAP_MS);
  shortBeep(buzLocal, 500);
#if USE_WIRED_REMOTE
  DBG_PRINTLN("[boot] Buzzer test (remote)");
  shortBeep(buzRemote, 500);
  delay(BEEP_GAP_MS);
  shortBeep(buzRemote, 500);
#endif

  DBG_PRINTLN("[boot] Self-test done");
}

// ---------------- Helpers ----------------
void resetSession() {
  state = State::READY;
  level = 1;
  runIndex = 0;
  targetMs = 0;
  for (uint8_t i = 0; i < RUNS_PER_LEVEL; i++) runTimesMs[i] = 0;
  calibStartedAt = 0;
  runStartedAt = 0;
  inOvertime = false;
  lastActivity = millis();
  showDashes();
  setBuzzContinuous(false);
  setRgb(0, 0, 40); // dim blue (awake/ready)
  totalLaps = 0;
  lastLapMs = 0;
  lastDiffMs = 0;
  statsPendingAt = 0;
  statsStepUntil = 0;
  statsPhase = 0;
  DBG_PRINTLN("[session] reset");
}

uint32_t clampTarget(uint32_t ms) {
  if (ms < MIN_TARGET_MS) return MIN_TARGET_MS;
  return ms;
}

// Drop the single fastest and single slowest run, then average the remainder.
// Works for any n >= 1; for n <= 2 it just returns the plain mean.
uint32_t trimmedMean(const uint32_t t[], uint8_t n) {
  if (n == 0) return 0;
  if (n == 1) return t[0];
  if (n == 2) return (t[0] + t[1]) / 2;
  uint32_t sum = 0;
  uint32_t mn = 0xFFFFFFFFu;
  uint32_t mx = 0;
  for (uint8_t i = 0; i < n; i++) {
    sum += t[i];
    if (t[i] < mn) mn = t[i];
    if (t[i] > mx) mx = t[i];
  }
  sum -= mn;
  sum -= mx;
  return sum / (uint32_t)(n - 2);
}

// ---------------- Setup ----------------
void setup() {
  DBG_BEGIN();
  delay(150);
  DBG_PRINTLN();
  DBG_PRINTLN("=== BeepTest boot ===");
  DBG_PRINTF("USE_WIRED_REMOTE=%d\n", USE_WIRED_REMOTE);
  DBG_PRINTF("Pins: TM1637 CLK=%u DIO=%u | BTN_START=%u BTN_REMOTE=%u | BUZZ_LOCAL=%u BUZZ_REMOTE=%u\n",
             PIN_TM1637_CLK, PIN_TM1637_DIO, PIN_BTN_START, PIN_BTN_REMOTE, PIN_BUZZER_LOCAL, PIN_BUZZER_REMOTE);

  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_REMOTE, INPUT_PULLUP);
  pinMode(PIN_BUZZER_LOCAL, OUTPUT);
  pinMode(PIN_BUZZER_REMOTE, OUTPUT);
  digitalWrite(PIN_BUZZER_LOCAL, LOW);
  digitalWrite(PIN_BUZZER_REMOTE, LOW);

  display.setBrightness(4, true);
  showBlank();
  setRgb(0, 0, 0);
  lastActivity = millis();

  startupSelfTest();
  DBG_PRINTLN("[boot] Ready. Press a button to wake.");
}

// ---------------- Main loop ----------------
void loop() {
  uint32_t now = millis();

  // Display breathing animation while actively running (helps when dots aren't wired).
  updateBrightnessPulse(false);

  updateButton(btnStart, now);
  updateButton(btnRemote, now);

  // Periodic raw pin level logging (helps diagnose wiring / wrong GPIO numbers).
  // To avoid the on-board UART TX LED constantly blinking when the device is
  // asleep, only log when something actually changed, OR every IDLE_RAW_LOG_MS,
  // and skip entirely while in OFF.
  static uint32_t lastRawLogAt = 0;
  static int      lastRawStart = -1;
  static int      lastRawRemote = -1;
  if (ENABLE_SERIAL_DEBUG && state != State::OFF) {
    int rs = digitalRead(PIN_BTN_START);
    int rr = digitalRead(PIN_BTN_REMOTE);
    bool changed = (rs != lastRawStart) || (rr != lastRawRemote);
    if (changed || (now - lastRawLogAt) > IDLE_RAW_LOG_MS) {
      lastRawLogAt = now;
      lastRawStart = rs;
      lastRawRemote = rr;
      DBG_PRINTF("[raw] BTN_START=%d BTN_REMOTE=%d\n", rs, rr);
    }
  }

  if (btnStart.fell) {
    DBG_PRINTLN("[btn] START pressed");
  }
  if (btnStart.rose) {
    DBG_PRINTLN("[btn] START released");
  }
  if (btnRemote.fell) {
    DBG_PRINTLN("[btn] REMOTE/2nd pressed");
  }

  // START button: short press vs long hold (long hold never counts as a lap)
  bool startShortPress = false;
  bool startLongPress = false;
  if (btnStart.fell) {
    startHoldBegan = now;
    startHoldHandled = false;
  }
  if (btnStart.stable && startHoldBegan != 0 && !startHoldHandled && (now - startHoldBegan) >= RESET_HOLD_MS) {
    startLongPress = true;
    startHoldHandled = true;
  }
  if (btnStart.rose) {
    if (startHoldBegan != 0 && !startHoldHandled) {
      startShortPress = true;
    }
    startHoldBegan = 0;
    startHoldHandled = false;
  }

  bool anyPress = startShortPress || btnRemote.fell;

  // Long hold behavior:
  // - while running: pause (and reset only the current lap)
  // - while paused: reset the whole session
  // - otherwise: reset session
  if (startLongPress) {
    if (state == State::RUN_ACTIVE) {
      DBG_PRINTLN("[flow] pause (lap reset)");
      state = State::PAUSED;
      setBuzzContinuous(false);
      inOvertime = false;
      runStartedAt = now; // reset the lap timer
      statsPendingAt = 0;
      statsStepUntil = 0;
      statsPhase = 0;
      showPaused();
      lastActivity = now;
      logStateIfChanged();
      return;
    }

    if (state == State::PAUSED) {
      DBG_PRINTLN("[flow] reset (from pause)");
      resetSession();
      return;
    }

    DBG_PRINTLN("[flow] reset");
    resetSession();
    return;
  }

  // inactivity handling
  if ((now - lastActivity) > OFF_AFTER_MS) {
    showBlank();
    state = State::OFF;
    setBuzzContinuous(false);
    setRgb(0, 0, 0);
    return;
  }

  switch (state) {
    case State::OFF:
      if (anyPress) {
        state = State::READY;
        showDashes();
        setBuzzContinuous(false);
        setRgb(0, 0, 40); // dim blue
        lastActivity = now;
        DBG_PRINTLN("[flow] wake");
      }
      break;

    case State::READY:
      // next press starts calibration run
      showDashes();
      setRgb(0, 0, 40); // dim blue
      if (anyPress) {
        calibStartedAt = now;
        state = State::CALIB_RUNNING;
        lastActivity = now;
        DBG_PRINTLN("[flow] calibration start");
        shortBeep(buzLocal, 300);
      }
      break;

    case State::CALIB_RUNNING:
      showTime(now - calibStartedAt);
      setRgb(0, 40, 40); // teal-ish
      if (anyPress) {
        uint32_t calibMs = now - calibStartedAt;
        DBG_PRINTF("[flow] calibration end: %lu ms\n", (unsigned long)calibMs);
        if (calibMs < MIN_LAP_MS) {
          // too short, treat as accidental
          shortBeep(buzLocal, 400);
          state = State::READY;
          break;
        }
        targetMs = clampTarget(calibMs > TARGET_BONUS_MS ? (calibMs - TARGET_BONUS_MS) : MIN_TARGET_MS);
        level = 1;
        runIndex = 0;
        for (uint8_t i = 0; i < RUNS_PER_LEVEL; i++) runTimesMs[i] = 0;
        startScrollLevel(level);
        state = State::SCROLL_LEVEL;
        lastActivity = now;
        DBG_PRINTF("[flow] Level 1 target set: %lu ms\n", (unsigned long)targetMs);
      }
      break;

    case State::SCROLL_LEVEL:
      setRgb(0, 0, 40); // dim blue
      if (anyPress) {
        // allow skipping the scroll
        runStartedAt = now;
        inOvertime = false;
        setBuzzContinuous(false);
        state = State::RUN_ACTIVE;
        lastActivity = now;
        DBG_PRINTLN("[flow] scroll skipped");
        break;
      }
      if (updateScroll(now)) {
        runStartedAt = now;
        inOvertime = false;
        setBuzzContinuous(false);
        state = State::RUN_ACTIVE;
        lastActivity = now;
      }
      break;

    case State::RUN_ACTIVE: {
      uint32_t elapsed = now - runStartedAt;
      bool colonOn = colonHeartbeatOn(now);

      // Stats overlay (after a press): lap time -> target -> diff, 0.5s each,
      // starting STATS_DELAY_MS after the press. The progress dots have already
      // updated immediately on press, so this overlay is the *secondary* feedback.
      if (statsPendingAt != 0 && now >= statsPendingAt) {
        if (statsStepUntil == 0) {
          statsPhase = 0;
          statsStepUntil = now + STATS_STEP_MS;
        } else if (now >= statsStepUntil) {
          statsPhase++;
          statsStepUntil = now + STATS_STEP_MS;
        }

        if (statsPhase == 0) {
          display.showNumberDec((uint16_t)min<uint32_t>(9999, (lastLapMs + 500) / 1000), true, 4, 0);
        } else if (statsPhase == 1) {
          display.showNumberDec((uint16_t)min<uint32_t>(9999, (targetMs + 500) / 1000), true, 4, 0);
        } else if (statsPhase == 2) {
          showDiffSeconds(lastDiffMs);
        } else {
          statsPendingAt = 0;
          statsStepUntil = 0;
          statsPhase = 0;
        }
      }

      // Decide buzzer / LED based on time vs target. Display is decided below.
      if (elapsed >= targetMs) {
        setBuzzContinuous(true);
        inOvertime = true;
      } else {
        setBuzzContinuous(false);
        inOvertime = false;
      }
      updateRunLed(elapsed, targetMs, now);

      // Display: progress dots are the primary "instant feedback" view.
      // Under COUNTDOWN_HINT_MS remaining, briefly alternate with the seconds countdown.
      // In overtime, flash the dots so it's clear something is wrong.
      if (statsPendingAt == 0) {
        if (inOvertime) {
          bool flashOn = ((now / OVERTIME_FLASH_MS) % 2) == 0;
          if (flashOn) {
            uint32_t overtimeSec = (elapsed - targetMs) / 1000;
            showCountdownOnly(overtimeSec, colonOn);
          } else {
            showProgress(runIndex, RUNS_PER_LEVEL);
          }
        } else {
          uint32_t remainingMs = targetMs - elapsed;
          uint32_t remainingSec = (remainingMs + 999) / 1000;
          if (remainingMs <= COUNTDOWN_HINT_MS) {
            // Show countdown just AFTER the half-second so the number stays stable while displayed.
            bool showCd = (remainingMs % 1000) > 500;
            if (showCd) {
              showCountdownOnly(remainingSec, colonOn);
            } else {
              showProgress(runIndex, RUNS_PER_LEVEL);
            }
          } else {
            showProgress(runIndex, RUNS_PER_LEVEL);
          }
        }
      }

      if (anyPress) {
        uint32_t runMs = now - runStartedAt;
        if (runMs < MIN_LAP_MS) {
          shortBeep(buzLocal, 350);
          DBG_PRINTLN("[run] ignored (too fast)");
          break;
        }
        runTimesMs[runIndex] = runMs;
        totalLaps++;
        lastLapMs = runMs;
        lastDiffMs = (int32_t)runMs - (int32_t)targetMs;
        statsPendingAt = now + STATS_DELAY_MS;
        statsStepUntil = 0;
        statsPhase = 0;
        DBG_PRINTF("[run] end #%u: %lu ms\n", (unsigned)(runIndex + 1), (unsigned long)runMs);
        runIndex++;
        lastActivity = now;
        setBuzzContinuous(false);
        inOvertime = false;

        // Update the progress dots IMMEDIATELY so the player sees their press registered,
        // independent of what the buzzers are doing.
        showProgress(runIndex, RUNS_PER_LEVEL);

        // Tiny "tick" on the local buzzer to confirm registration. Distinct from the
        // long overtime tone and the long level-up tone. Set LAP_CONFIRM_MS = 0 to mute.
        if (LAP_CONFIRM_MS > 0) {
          shortBeep(buzLocal, LAP_CONFIRM_MS);
        }

        if (runIndex < RUNS_PER_LEVEL) {
          runStartedAt = now;
          DBG_PRINTF("[run] start #%u\n", (unsigned)(runIndex + 1));
          state = State::RUN_ACTIVE;
        } else {
          uint32_t avg = trimmedMean(runTimesMs, RUNS_PER_LEVEL);
          DBG_PRINTF("[level] trimmed avg: %lu ms (target %lu)\n", (unsigned long)avg, (unsigned long)targetMs);

          bool faster = avg < targetMs;
          if (faster) {
            level++;
            targetMs = clampTarget(avg > TARGET_BONUS_MS ? (avg - TARGET_BONUS_MS) : MIN_TARGET_MS);
            DBG_PRINTF("[level] UP -> %u, new target %lu ms\n", (unsigned)level, (unsigned long)targetMs);
            shortBeep(buzLocal, LEVEL_UP_BEEP_MS);
            startScrollLevel(level);
            state = State::SCROLL_LEVEL;
          } else {
            DBG_PRINTF("[level] stay at %u\n", (unsigned)level);
            startScrollLevel(level);
            state = State::SCROLL_LEVEL;
          }

          runIndex = 0;
          for (uint8_t i = 0; i < RUNS_PER_LEVEL; i++) runTimesMs[i] = 0;
        }
      }
      break;
    }

    case State::PAUSED:
      showPaused();
      setBuzzContinuous(false);
      updatePausedLed(now);
      if (anyPress) {
        DBG_PRINTLN("[flow] resume");
        runStartedAt = now; // lap restarts from 0
        inOvertime = false;
        statsPendingAt = 0;
        statsStepUntil = 0;
        statsPhase = 0;
        state = State::RUN_ACTIVE;
        lastActivity = now;
      }
      break;

    case State::SHOW_SUMMARY:
      // reserved (kept in enum in case we want to show avg/target later)
      runStartedAt = now;
      inOvertime = false;
      setBuzzContinuous(false);
      state = State::RUN_ACTIVE;
      break;
  }

  logStateIfChanged();
}

