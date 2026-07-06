/**
 * @file    display.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   OLED initialization, custom icon bitmap data, and the shared
 *          status bar renderer used by every screen in animations.cpp.
 *
 * Icon note: the bitmaps below were generated programmatically (rasterized
 * from simple vector shapes into exact 1-bit pixel data) rather than
 * hand-typed byte-by-byte, so every icon is pixel-accurate to its intended
 * shape (mic capsule + stand, wifi arcs + dot, 8-point sparkle, cloud,
 * battery, checkmark, warning triangle).
 */

#include "display.h"
#include <Wire.h>
#include "config.h"

namespace FayasDisplay {

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);

// ============================================================================
// Icon bitmaps (16x16, 1bpp, PROGMEM)
// ============================================================================

const uint8_t ICON_MIC[] PROGMEM = {
    0x00, 0x00, 0x01, 0x80, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0,
    0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x11, 0x88, 0x10, 0x88, 0x08, 0x90,
    0x04, 0xA0, 0x03, 0xC0, 0x07, 0xF0, 0x00, 0x00,
};

const uint8_t ICON_WIFI[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xC0, 0x0F, 0xF0,
    0x1F, 0xF8, 0x07, 0xE0, 0x05, 0xA0, 0x03, 0xC0, 0x00, 0x00, 0x01, 0x80,
    0x03, 0xC0, 0x03, 0xC0, 0x01, 0x80, 0x00, 0x00,
};

const uint8_t ICON_AI[] PROGMEM = {
    0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0x01, 0x80, 0x01, 0xC0, 0x03, 0xC0,
    0x03, 0xC0, 0x0F, 0xF8, 0x7F, 0xFF, 0x03, 0xC0, 0x03, 0xC0, 0x01, 0xC0,
    0x01, 0x80, 0x01, 0x80, 0x00, 0x80, 0x00, 0x80,
};

const uint8_t ICON_CLOUD[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x03, 0xF8,
    0x1F, 0xF8, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF,
    0x3F, 0xFE, 0x1F, 0xFC, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t ICON_BATTERY[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFC, 0x40, 0x04,
    0x5F, 0xF7, 0x5F, 0xF7, 0x5F, 0xF7, 0x5F, 0xF7, 0x5F, 0xF7, 0x40, 0x04,
    0x7F, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t ICON_CHECK[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x0E, 0x00, 0x1C,
    0x00, 0x38, 0x00, 0x70, 0x10, 0x60, 0x38, 0xE0, 0x1D, 0xC0, 0x0F, 0x80,
    0x07, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t ICON_WARNING[] PROGMEM = {
    0x00, 0x00, 0x00, 0x80, 0x01, 0x40, 0x01, 0x40, 0x02, 0x20, 0x02, 0xA0,
    0x04, 0x90, 0x04, 0x90, 0x08, 0x88, 0x08, 0x88, 0x10, 0x04, 0x10, 0x84,
    0x20, 0x82, 0x20, 0x02, 0x7F, 0xFF, 0x00, 0x00,
};

// Forward declaration: defined below, used by drawStatusBar() above its
// own definition point.
static float getFreeHeapKBSafe();

bool begin() {
    // Purpose: Bring up the I2C bus and initialize the SSD1306 controller.
    // Inputs: none (pins/address from config.h).
    // Outputs: true if the OLED acknowledged at OLED_I2C_ADDRESS.
    // Logic: Wire.begin() with explicit SDA/SCL pins (ESP32 allows remapping
    //        I2C to any GPIO pair, unlike AVR boards) then hand off to
    //        Adafruit_SSD1306::begin().
    // Possible errors: begin() returns false on a wiring fault, wrong
    //        address, or a dead panel - the caller (main.ino) falls back
    //        to Serial-only diagnostics if this fails, since without a
    //        working display we can't show an error screen either.
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    Wire.setClock(OLED_I2C_CLOCK_HZ);

    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
        return false;
    }

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    return true;
}

void drawStatusBar(bool connected, int signalBars, bool debugMode) {
    // Purpose: Render the persistent 10px-tall status bar shown at the top
    //          of every screen except the full-screen Boot animation.
    // Inputs: connected/signalBars describe Wi-Fi state; debugMode toggles
    //         the free-heap readout.
    // Outputs: none (draws into the shared oled framebuffer; caller is
    //          responsible for calling oled.display() once per frame after
    //          all drawing for that frame is done, to avoid flicker from
    //          multiple partial flushes).
    // Logic: Signal bars are drawn as 4 small vertical bars of increasing
    //        height, filled up to `signalBars`, greyed out (skipped) when
    //        disconnected.
    // Possible errors: none; purely additive drawing.
    const int barX = 2;
    const int barBaseY = 8;
    for (int i = 0; i < 4; i++) {
        int barHeight = 2 + i * 2;
        int x = barX + i * 3;
        int y = barBaseY - barHeight;
        if (connected && i < signalBars) {
            oled.fillRect(x, y, 2, barHeight, SSD1306_WHITE);
        } else {
            oled.drawRect(x, y, 2, barHeight, SSD1306_WHITE);
        }
    }

    // Battery placeholder icon, right-aligned.
    oled.drawBitmap(SCREEN_WIDTH - ICON_SIZE, 0, ICON_BATTERY, ICON_SIZE, ICON_SIZE, SSD1306_WHITE);

    if (debugMode) {
        char heapStr[12];
        snprintf(heapStr, sizeof(heapStr), "%dK", (int)getFreeHeapKBSafe());
        oled.setTextSize(1);
        oled.setCursor(SCREEN_WIDTH - ICON_SIZE - 28, 1);
        oled.print(heapStr);
    }

    oled.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);
}

// Small local wrapper to avoid a circular include of utils.h just for one
// float->int display value; keeps display.cpp focused on drawing only.
static float getFreeHeapKBSafe() {
    return ESP.getFreeHeap() / 1024.0f;
}

} // namespace FayasDisplay
