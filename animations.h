/**
 * @file    animations.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   All OLED screen rendering: boot sequence, home idle screen,
 *          listening/thinking/response/error/success screens, and the
 *          reusable visual-effect primitives (fade, particles, pulse,
 *          typewriter, scrolling, spinners, progress bars) that back them.
 *
 * Design: every render*() function is called once per animation frame
 * from main.ino's loop() and draws exactly one frame into the shared
 * Adafruit_SSD1306 buffer based on elapsed time (millis()), never using
 * delay(). This is what keeps 60fps-target animation smooth while
 * concurrently polling the button and (during LISTENING) pumping audio.
 */

#ifndef ANIMATIONS_H
#define ANIMATIONS_H

#include <Arduino.h>
#include "utils.h"

namespace FayasAnimations {

/// Call once at startup after FayasDisplay::begin() succeeds.
void begin();

/**
 * @brief Reset per-screen animation state (elapsed timers, particle
 *        positions, typewriter progress, etc.) whenever the app transitions
 *        into a new AppState. Call exactly once on every state change.
 */
void onEnterState(AppState newState);

/// Render one frame of the boot/splash sequence.
/// @return true once the boot sequence has fully completed.
bool renderBoot();

/// Render one frame of the idle Home screen (particles, breathing, icons).
/// @param wifiConnected  Current Wi-Fi state, for the status bar.
/// @param signalBars     0-3 Wi-Fi signal strength.
/// @param debugMode      Whether to show the free-heap debug overlay.
void renderHome(bool wifiConnected, int signalBars, bool debugMode);

/// Render one frame of the Listening screen.
/// @param elapsedMs Recording elapsed time, for the mm:ss timer.
void renderListening(unsigned long elapsedMs);

/// Render one frame of the Thinking screen (spinner, orbit, loading bar).
void renderThinking();

/**
 * @brief Render one frame of the Response screen: typewriter reveal of
 *        `fullText`, then paginated auto-scroll once fully revealed.
 * @param fullText     Complete response text to display.
 * @param apiLatencyMs Latency of the completed API call, shown briefly.
 * @return true once the response has been fully shown (all pages viewed
 *         at least once), signalling main.ino it may move to SUCCESS.
 */
bool renderResponse(const String &fullText, unsigned long apiLatencyMs);

/**
 * @brief Render one frame of the Error screen.
 * @param message Two-line error message (title\nsubtitle), e.g. from
 *                 FayasAI::resultToMessage().
 */
void renderError(const char *message);

/// Render one frame of the Success checkmark screen.
/// @return true once the success animation has completed.
bool renderSuccess();

} // namespace FayasAnimations

#endif // ANIMATIONS_H
