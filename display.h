/**
 * @file    display.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   Thin wrapper around Adafruit_SSD1306 providing initialization,
 *          a shared display object for animations.cpp, custom PROGMEM
 *          icon bitmaps, and a reusable status bar renderer.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace FayasDisplay {

/// Shared display instance, defined in display.cpp. animations.cpp draws
/// directly onto this object; display.cpp owns initialization only, since
/// splitting "who owns the buffer" from "who draws each frame" would add
/// indirection without benefit in a single-display embedded project.
extern Adafruit_SSD1306 oled;

/// Icon bitmap dimensions - all custom icons are 16x16 1-bit glyphs.
static const uint8_t ICON_SIZE = 16;

// Custom PROGMEM icon bitmaps (generated as exact pixel data, not
// hand-guessed bytes - see project tooling notes in README).
extern const uint8_t ICON_MIC[] PROGMEM;
extern const uint8_t ICON_WIFI[] PROGMEM;
extern const uint8_t ICON_AI[] PROGMEM;
extern const uint8_t ICON_CLOUD[] PROGMEM;
extern const uint8_t ICON_BATTERY[] PROGMEM;
extern const uint8_t ICON_CHECK[] PROGMEM;
extern const uint8_t ICON_WARNING[] PROGMEM;

/**
 * @brief Initialize the I2C bus and SSD1306 controller.
 * @return true if the display responded at OLED_I2C_ADDRESS, false
 *         otherwise (e.g. wiring fault or wrong address).
 */
bool begin();

/**
 * @brief Draw the persistent top status bar: Wi-Fi signal bars, a small
 *        battery placeholder icon, and (in debug mode) free heap text.
 * @param connected    Current Wi-Fi connection state.
 * @param signalBars   0-3, from utils::rssiToBars().
 * @param debugMode    If true, also renders free-heap text on the right.
 */
void drawStatusBar(bool connected, int signalBars, bool debugMode);

} // namespace FayasDisplay

#endif // DISPLAY_H
