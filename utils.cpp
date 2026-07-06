/**
 * @file    utils.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of shared timing, input-debouncing, and
 *          formatting helpers.
 */

#include "utils.h"

// ============================================================================
// NonBlockingTimer
// ============================================================================

NonBlockingTimer::NonBlockingTimer(unsigned long intervalMs)
    : _intervalMs(intervalMs), _lastTriggerMs(0) {}

bool NonBlockingTimer::ready() {
    // Purpose: Non-blocking interval check.
    // Inputs: none (reads millis()).
    // Outputs: true if >= _intervalMs has passed since the last trigger.
    // Logic: Unsigned subtraction of millis() values is rollover-safe by
    //        construction in C/C++ (wraps correctly even when millis()
    //        overflows back to 0 after ~49.7 days).
    // Possible errors: none; this function cannot fail.
    unsigned long now = millis();
    if ((now - _lastTriggerMs) >= _intervalMs) {
        _lastTriggerMs = now;
        return true;
    }
    return false;
}

void NonBlockingTimer::reset() {
    _lastTriggerMs = millis();
}

void NonBlockingTimer::setInterval(unsigned long intervalMs) {
    _intervalMs = intervalMs;
}

// ============================================================================
// DebouncedButton
// ============================================================================

DebouncedButton::DebouncedButton(uint8_t pin, bool activeLow, unsigned long debounceMs)
    : _pin(pin),
      _activeLow(activeLow),
      _debounceMs(debounceMs),
      _stableState(false),
      _lastRawState(false),
      _lastChangeMs(0),
      _pressedEdge(false),
      _releasedEdge(false) {}

void DebouncedButton::begin() {
    // Purpose: Configure the GPIO pin for reading the push button.
    // Inputs: none. Outputs: none.
    // Logic: Use the internal pull-up so a single wire to GND (active-low)
    //        is sufficient wiring; avoids a floating-pin misread.
    // Possible errors: none for a valid GPIO number.
    // Use internal pull-up for bare active-low buttons (wire to GND).
    // For active-high touch switch modules (which drive their own output),
    // use plain INPUT so the pull-up doesn't interfere with the module.
    pinMode(_pin, _activeLow ? INPUT_PULLUP : INPUT);
    bool rawPressed = _activeLow ? (digitalRead(_pin) == LOW)
                                  : (digitalRead(_pin) == HIGH);
    _stableState = rawPressed;
    _lastRawState = rawPressed;
    _lastChangeMs = millis();
}

void DebouncedButton::update() {
    // Purpose: Poll the raw pin and update the debounced logical state.
    // Inputs: none (reads digitalRead()).
    // Outputs: updates _stableState and sets edge flags for exactly one
    //          update() call after a transition.
    // Logic: A raw reading must remain stable for _debounceMs before it is
    //        accepted, filtering out mechanical contact bounce.
    // Possible errors: none; worst case is a missed/delayed edge if called
    //          less often than the debounce window, which is a caller
    //          responsibility (call update() every loop iteration).
    _pressedEdge = false;
    _releasedEdge = false;

    bool rawPressed = _activeLow ? (digitalRead(_pin) == LOW)
                                  : (digitalRead(_pin) == HIGH);
    unsigned long now = millis();

    if (rawPressed != _lastRawState) {
        _lastRawState = rawPressed;
        _lastChangeMs = now;
    }

    if ((now - _lastChangeMs) >= _debounceMs && rawPressed != _stableState) {
        _stableState = rawPressed;
        if (_stableState) {
            _pressedEdge = true;
        } else {
            _releasedEdge = true;
        }
    }
}

bool DebouncedButton::wasPressed() const {
    return _pressedEdge;
}

bool DebouncedButton::wasReleased() const {
    return _releasedEdge;
}

bool DebouncedButton::isPressed() const {
    return _stableState;
}

// ============================================================================
// Formatting / diagnostics
// ============================================================================

const char *formatElapsedTime(unsigned long elapsedMs) {
    // Purpose: Render an elapsed duration as "M:SS" for the recording timer.
    // Inputs: elapsedMs - elapsed milliseconds.
    // Outputs: pointer to a static internal buffer holding the formatted
    //          string (valid until the next call; not reentrant/thread-safe,
    //          which is acceptable on a single-core Arduino sketch loop).
    // Logic: Simple integer division into minutes/seconds.
    // Possible errors: none for reasonable input ranges (< ~99 minutes).
    static char buffer[8];
    unsigned long totalSeconds = elapsedMs / 1000UL;
    unsigned long minutes = totalSeconds / 60UL;
    unsigned long seconds = totalSeconds % 60UL;
    snprintf(buffer, sizeof(buffer), "%lu:%02lu", minutes, seconds);
    return buffer;
}

float getFreeHeapKB() {
    // Purpose: Report free heap for the debug overlay.
    // Inputs: none. Outputs: free heap in kilobytes.
    // Logic: ESP.getFreeHeap() returns bytes; convert to KB for display.
    // Possible errors: none.
    return ESP.getFreeHeap() / 1024.0f;
}

int rssiToBars(long rssiDbm) {
    // Purpose: Convert a Wi-Fi RSSI reading into a 0-3 bar count for the
    //          status bar icon.
    // Inputs: rssiDbm - signal strength in dBm (negative values; closer to
    //         0 is stronger, e.g. -40 is excellent, -85 is very weak).
    // Outputs: integer 0..3.
    // Logic: Simple threshold ladder tuned for typical home Wi-Fi ranges.
    // Possible errors: none; out-of-range values clamp to the nearest bucket.
    if (rssiDbm >= -55) return 3;
    if (rssiDbm >= -70) return 2;
    if (rssiDbm >= -85) return 1;
    return 0;
}
