#pragma once

#include <Arduino.h>

// Keep sketch-specific types in a header so Arduino's auto-generated
// function prototypes (added near the top of the .ino) can see them.

struct DebouncedButton {
  uint8_t pin;
  bool stable = false;       // true when pressed (active-low)
  bool lastReading = false;
  uint32_t changedAt = 0;
  bool fell = false;         // went from released to pressed
  bool rose = false;         // went from pressed to released
};

struct Buzzer {
  uint8_t pin;
};

// State enum lives in a header so Arduino's auto-generated prototypes can use it.
enum class State : uint8_t {
  OFF,            // display blank, waiting for wake press
  READY,          // awake, waiting for "go" press
  CALIB_RUNNING,  // timing the calibration run (sets Level 1 basis)
  SCROLL_LEVEL,   // scrolling "LEVEL n" message
  RUN_ACTIVE,     // run in progress (shows run# and countdown seconds)
  PAUSED,         // paused; current lap is reset; press to resume; hold to reset session
  SHOW_SUMMARY,   // short status display after 5 runs (level stays or increases)
};
