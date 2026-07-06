/**
 * @file    utils.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   Shared types, the application state machine enum, and small
 *          non-blocking timing/formatting helpers used across modules.
 */

#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>

// ----------------------------------------------------------------------------
// Application state machine
// ----------------------------------------------------------------------------
// The entire assistant behaves as a strict state machine. Exactly one state
// is active at a time, and animations.cpp renders each state differently.
// main.ino's loop() is responsible for the state transitions listed in the
// project's user-flow diagram (Boot -> Home -> Listening -> Thinking ->
// Response -> Success -> Home, with Error reachable from any network step).
enum class AppState : uint8_t {
    BOOT,        // Startup animation, runs once
    HOME,        // Idle screen, waiting for button press
    LISTENING,   // Button held, actively recording audio
    THINKING,    // Audio sent to AI, waiting for a response
    RESPONSE,    // Displaying the AI text response (typewriter + scroll)
    SUCCESS,     // Brief checkmark animation before returning home
    ERROR_STATE  // Wi-Fi lost, API error, or other recoverable failure
};

// ----------------------------------------------------------------------------
// Non-blocking timer
// ----------------------------------------------------------------------------
// Purpose: Replaces delay()-based timing with a millis()-based helper so
// animations and I/O polling never block the CPU. Call ready() each loop;
// it returns true at most once per interval.
class NonBlockingTimer {
public:
    /**
     * @brief Construct a timer with a given interval.
     * @param intervalMs How often (in ms) ready() should return true.
     */
    explicit NonBlockingTimer(unsigned long intervalMs);

    /**
     * @brief Check whether the configured interval has elapsed.
     * @return true exactly once per elapsed interval; false otherwise.
     * @note Uses unsigned subtraction so it safely handles millis() rollover
     *       (which occurs roughly every 49.7 days of continuous uptime).
     */
    bool ready();

    /// Reset the timer's reference point to "now" without waiting.
    void reset();

    /// Change the interval at runtime (e.g. slower FPS in screensaver mode).
    void setInterval(unsigned long intervalMs);

private:
    unsigned long _intervalMs;
    unsigned long _lastTriggerMs;
};

// ----------------------------------------------------------------------------
// Debounced button reader
// ----------------------------------------------------------------------------
// Purpose: Provides a clean, debounced press/release edge detector for the
// push-to-talk button without ever calling delay().
class DebouncedButton {
public:
    /**
     * @param pin        GPIO pin the button is wired to.
     * @param activeLow  true if pressed == LOW (typical with INPUT_PULLUP).
     * @param debounceMs Minimum stable time before a state change is trusted.
     */
    DebouncedButton(uint8_t pin, bool activeLow, unsigned long debounceMs);

    /// Call once in setup() to configure the pin mode.
    void begin();

    /// Call every loop() iteration to update internal debounced state.
    void update();

    /// True on the single loop iteration the button transitioned to pressed.
    bool wasPressed() const;

    /// True on the single loop iteration the button transitioned to released.
    bool wasReleased() const;

    /// True for as long as the debounced state is "pressed".
    bool isPressed() const;

private:
    uint8_t _pin;
    bool _activeLow;
    unsigned long _debounceMs;

    bool _stableState;      // Debounced, trusted logical state (true = pressed)
    bool _lastRawState;     // Last raw pin reading, used to detect bounces
    unsigned long _lastChangeMs;

    bool _pressedEdge;
    bool _releasedEdge;
};

// ----------------------------------------------------------------------------
// Formatting / diagnostics helpers
// ----------------------------------------------------------------------------

/**
 * @brief Format elapsed milliseconds as "M:SS" for the recording timer.
 * @param elapsedMs Elapsed time in milliseconds.
 * @return A short-lived C-string valid until the next call (static buffer).
 */
const char *formatElapsedTime(unsigned long elapsedMs);

/**
 * @brief Return the current free heap in kilobytes, for the debug overlay.
 */
float getFreeHeapKB();

/**
 * @brief Map a Wi-Fi RSSI value (dBm) to a 0-3 signal bar count for the
 *        Wi-Fi status icon.
 * @param rssiDbm Signal strength in dBm (typically -30 to -90).
 * @return Integer 0 (no/weak signal) through 3 (excellent signal).
 */
int rssiToBars(long rssiDbm);

#endif // UTILS_H
