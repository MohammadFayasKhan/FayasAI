/**
 * @file    animations.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of every OLED screen and the visual-effect
 *          primitives they're built from. All timing is millis()-based;
 *          nothing in this file ever calls delay().
 */

#include "animations.h"
#include "display.h"
#include <math.h>
#include <esp_system.h>

using FayasDisplay::oled;
using FayasDisplay::ICON_SIZE;

namespace FayasAnimations {

// ============================================================================
// Shared / cross-screen constants
// ============================================================================
static const float PI_F = 3.14159265f;
static const int CONTENT_TOP = 12; // Below the 10px status bar + 1px gap

// ============================================================================
// Floating particle field (used on Home and Thinking screens)
// ============================================================================
struct Particle {
    float x, y;
    float speed;
    uint8_t size;
};
static const int PARTICLE_COUNT = 6;
static Particle s_particles[PARTICLE_COUNT];

static void initParticles() {
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        s_particles[i].x = (float)(random(0, 128));
        s_particles[i].y = (float)(random(CONTENT_TOP, 64));
        s_particles[i].speed = 0.15f + (random(0, 20) / 100.0f);
        s_particles[i].size = 1;
    }
}

static void stepAndDrawParticles(unsigned long dtMs) {
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        s_particles[i].y -= s_particles[i].speed * (dtMs / 16.0f);
        if (s_particles[i].y < CONTENT_TOP) {
            s_particles[i].y = 63;
            s_particles[i].x = (float)(random(0, 128));
        }
        oled.drawPixel((int)s_particles[i].x, (int)s_particles[i].y, SSD1306_WHITE);
    }
}

// ============================================================================
// State-entry bookkeeping
// ============================================================================
static unsigned long s_stateEnterMs = 0;
static unsigned long s_lastFrameMs = 0;

void begin() {
    randomSeed((uint32_t)esp_random());
    initParticles();
    s_lastFrameMs = millis();
}

void onEnterState(AppState newState) {
    s_stateEnterMs = millis();
    // Per-screen state resets are handled lazily inside each render*()
    // function by comparing against s_stateEnterMs where needed (see
    // renderResponse's text-change detection for the one screen that
    // needs additional reset logic beyond a timestamp).
}

static unsigned long dtMsSinceLastFrame() {
    // Clamp dt so a large gap between frames (e.g. returning to a
    // particle-based screen after a long stretch in Thinking/Response)
    // doesn't cause particles to visibly "teleport" on the first frame.
    unsigned long now = millis();
    unsigned long dt = now - s_lastFrameMs;
    s_lastFrameMs = now;
    if (dt > 100UL) dt = 100UL;
    return dt;
}

// ============================================================================
// BOOT SCREEN
// ============================================================================
// Phase timeline (ms from state entry):
//   0    - 500  : glowing dot fades/grows in, centered
//   500  - 1000 : dot expands into a ring burst
//   1000 - 1900 : "Fayas AI" logo text draws in with an underline sweep
//   1900 - 2700 : "AI Assistant" subtitle fades in
//   2700 - 3200 : hold, then done
static const unsigned long BOOT_DOT_END = 500;
static const unsigned long BOOT_RING_END = 1000;
static const unsigned long BOOT_LOGO_END = 1900;
static const unsigned long BOOT_SUBTITLE_END = 2700;
static const unsigned long BOOT_TOTAL = 3200;

bool renderBoot() {
    unsigned long elapsed = millis() - s_stateEnterMs;
    oled.clearDisplay();

    if (elapsed < BOOT_DOT_END) {
        float t = elapsed / (float)BOOT_DOT_END;
        int radius = 1 + (int)(t * 4);
        oled.fillCircle(64, 28, radius, SSD1306_WHITE);
    } else if (elapsed < BOOT_RING_END) {
        float t = (elapsed - BOOT_DOT_END) / (float)(BOOT_RING_END - BOOT_DOT_END);
        oled.fillCircle(64, 28, 5, SSD1306_WHITE);
        int ringRadius = 5 + (int)(t * 18);
        oled.drawCircle(64, 28, ringRadius, SSD1306_WHITE);
    } else if (elapsed < BOOT_LOGO_END) {
        float t = (elapsed - BOOT_RING_END) / (float)(BOOT_LOGO_END - BOOT_RING_END);
        oled.setTextSize(2);
        oled.setCursor(14, 20);
        oled.print("Fayas AI");
        int lineWidth = (int)(t * 100);
        oled.drawFastHLine(14, 40, lineWidth, SSD1306_WHITE);
    } else if (elapsed < BOOT_SUBTITLE_END) {
        oled.setTextSize(2);
        oled.setCursor(14, 20);
        oled.print("Fayas AI");
        oled.drawFastHLine(14, 40, 100, SSD1306_WHITE);

        float t = (elapsed - BOOT_LOGO_END) / (float)(BOOT_SUBTITLE_END - BOOT_LOGO_END);
        // Simple fade-in simulated by dithering: show subtitle once t > 0.3
        // (true alpha blending isn't possible on a 1-bit display, so we
        // approximate a "fade" as a slightly delayed appearance following
        // the logo, which reads as a fade in the context of the full
        // boot sequence's motion).
        if (t > 0.3f) {
            oled.setTextSize(1);
            oled.setCursor(20, 50);
            oled.print("AI Assistant");
        }
    } else if (elapsed < BOOT_TOTAL) {
        oled.setTextSize(2);
        oled.setCursor(14, 20);
        oled.print("Fayas AI");
        oled.drawFastHLine(14, 40, 100, SSD1306_WHITE);
        oled.setTextSize(1);
        oled.setCursor(20, 50);
        oled.print("AI Assistant");
    }

    oled.display();
    return elapsed >= BOOT_TOTAL;
}

// ============================================================================
// HOME SCREEN
// ============================================================================
void renderHome(bool wifiConnected, int signalBars, bool debugMode) {
    unsigned long elapsed = millis() - s_stateEnterMs;
    unsigned long dt = dtMsSinceLastFrame();
    oled.clearDisplay();

    FayasDisplay::drawStatusBar(wifiConnected, signalBars, debugMode);
    stepAndDrawParticles(dt);

    // Breathing effect: gently scale the "Ready" text's vertical position
    // using a slow sine wave so the idle screen never looks frozen.
    float breathe = sinf(elapsed / 600.0f) * 2.0f;

    oled.setTextSize(2);
    oled.setCursor(18, (int)(20 + breathe));
    oled.print("Fayas AI");

    oled.setTextSize(1);
    oled.setCursor(46, 40);
    oled.print("Ready");

    // Blinking mic icon: visible for 700ms, hidden for 300ms, on a 1s cycle.
    if ((elapsed % 1000UL) < 700UL) {
        oled.drawBitmap(4, 46, FayasDisplay::ICON_MIC, ICON_SIZE, ICON_SIZE, SSD1306_WHITE);
    }

    oled.setCursor(24, 56);
    oled.print("Press Button");

    oled.display();
}

// ============================================================================
// LISTENING SCREEN
// ============================================================================
void renderListening(unsigned long elapsedMs) {
    unsigned long dt = dtMsSinceLastFrame();
    (void)dt;
    oled.clearDisplay();

    const int cx = 64, cy = 30;

    // Expanding, fading concentric sound-wave rings. Three rings at phase
    // offsets create a continuous "pulse" rather than a single ripple.
    for (int ring = 0; ring < 3; ring++) {
        unsigned long phase = (elapsedMs + ring * 300UL) % 900UL;
        float t = phase / 900.0f;
        int radius = 8 + (int)(t * 22);
        if (t < 0.85f) { // Skip drawing the ring right at reset for a cleaner loop
            oled.drawCircle(cx, cy, radius, SSD1306_WHITE);
        }
    }

    // Mic icon gently bouncing vertically.
    float bounce = sinf(elapsedMs / 200.0f) * 3.0f;
    oled.drawBitmap(cx - ICON_SIZE / 2, (int)(cy - ICON_SIZE / 2 + bounce),
                     FayasDisplay::ICON_MIC, ICON_SIZE, ICON_SIZE, SSD1306_WHITE);

    // Animated waveform bars beneath the mic.
    const int barCount = 9;
    const int barBaseY = 52;
    for (int i = 0; i < barCount; i++) {
        float phase = (elapsedMs / 90.0f) + i * 0.8f;
        int barHeight = 2 + (int)((sinf(phase) * 0.5f + 0.5f) * 8);
        int x = 30 + i * 8;
        oled.drawFastVLine(x, barBaseY - barHeight, barHeight, SSD1306_WHITE);
    }

    // "Listening" with animated trailing dots (cycles 0-3 dots every 1.2s).
    int dotCount = (int)((elapsedMs / 300UL) % 4);
    String label = "Listening";
    for (int i = 0; i < dotCount; i++) label += ".";
    oled.setTextSize(1);
    oled.setCursor(4, 2);
    oled.print(label);

    // Recording timer, right-aligned.
    oled.setCursor(100, 2);
    oled.print(formatElapsedTime(elapsedMs));

    oled.display();
}

// ============================================================================
// THINKING SCREEN
// ============================================================================
void renderThinking() {
    unsigned long elapsed = millis() - s_stateEnterMs;
    unsigned long dt = dtMsSinceLastFrame();
    oled.clearDisplay();

    stepAndDrawParticles(dt);

    const int cx = 64, cy = 26, orbitRadius = 14;

    // Orbit animation: a small dot circling a static AI sparkle icon.
    oled.drawBitmap(cx - ICON_SIZE / 2, cy - ICON_SIZE / 2, FayasDisplay::ICON_AI,
                     ICON_SIZE, ICON_SIZE, SSD1306_WHITE);

    float angle = (elapsed / 500.0f) * 2.0f * PI_F;
    int dotX = cx + (int)(cosf(angle) * orbitRadius);
    int dotY = cy + (int)(sinf(angle) * orbitRadius * 0.5f); // Flattened for a 3D-ish orbit
    oled.fillCircle(dotX, dotY, 2, SSD1306_WHITE);

    // Rotating arc "spinner" ring around the orbit for extra motion.
    for (int i = 0; i < 3; i++) {
        float a = angle + i * (2.0f * PI_F / 3.0f);
        int x = cx + (int)(cosf(a) * (orbitRadius + 6));
        int y = cy + (int)(sinf(a) * (orbitRadius + 6) * 0.5f);
        oled.drawPixel(x, y, SSD1306_WHITE);
    }

    // Typing dots cycling under a fixed label.
    int dotCount = (int)((elapsed / 350UL) % 4);
    String label = "AI is Thinking";
    for (int i = 0; i < dotCount; i++) label += ".";
    oled.setTextSize(1);
    oled.setCursor(2, 44);
    oled.print(label);

    // Looping loading bar (fills, then resets) - communicates "working"
    // without implying a fake, inaccurate percentage of real progress.
    const int barX = 4, barY = 56, barW = 120, barH = 5;
    oled.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    int fillW = (int)((elapsed % 1600UL) / 1600.0f * (barW - 2));
    oled.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);

    oled.display();
}

// ============================================================================
// RESPONSE SCREEN (typewriter + pagination)
// ============================================================================
static const int CHARS_PER_LINE = 21;
static const int LINES_PER_PAGE = 4;
static const int MAX_RESPONSE_LINES = 40;
static const unsigned long TYPEWRITER_CHARS_PER_SEC = 45;
static const unsigned long PAGE_PAUSE_MS = 2500;
static const unsigned long PAGE_TRANSITION_MS = 250;

// Forward declaration: defined near the end of this section, used inside
// renderResponse() above its own definition point.
static int SCREEN_WIDTH_LOCAL();

static String s_responseLines[MAX_RESPONSE_LINES];
static int s_responseLineCount = 0;
static int s_typewriterTargetChars = 0;
static String s_lastResponseText = "";
static unsigned long s_typewriterStartMs = 0;
static int s_currentPage = 0;
static unsigned long s_pageStartMs = 0;
static bool s_paginating = false;

/**
 * @brief Greedy word-wrap of `text` into fixed-width lines, stored into
 *        s_responseLines[]. Truncates with an ellipsis if the response is
 *        longer than MAX_RESPONSE_LINES lines can hold (a generous cap
 *        given AI_SYSTEM_PROMPT already asks for a short reply).
 */
static void wrapResponseText(const String &text) {
    s_responseLineCount = 0;
    String currentLine = "";
    int wordStart = 0;
    int len = text.length();
    int i = 0;

    while (i <= len && s_responseLineCount < MAX_RESPONSE_LINES) {
        bool atBreak = (i == len) || (text[i] == ' ') || (text[i] == '\n');
        if (atBreak) {
            String word = text.substring(wordStart, i);
            bool isNewlineBreak = (i < len && text[i] == '\n');

            if (word.length() > 0) {
                String candidate = currentLine.length() == 0 ? word : (currentLine + " " + word);
                if ((int)candidate.length() <= CHARS_PER_LINE) {
                    currentLine = candidate;
                } else {
                    if (currentLine.length() > 0) {
                        s_responseLines[s_responseLineCount++] = currentLine;
                    }
                    currentLine = word;
                }
            }
            if (isNewlineBreak && s_responseLineCount < MAX_RESPONSE_LINES) {
                s_responseLines[s_responseLineCount++] = currentLine;
                currentLine = "";
            }
            wordStart = i + 1;
        }
        i++;
    }
    if (currentLine.length() > 0 && s_responseLineCount < MAX_RESPONSE_LINES) {
        s_responseLines[s_responseLineCount++] = currentLine;
    }
}

static void resetResponseAnimationState(const String &text) {
    wrapResponseText(text);

    int firstPageLines = min(LINES_PER_PAGE, s_responseLineCount);
    int chars = 0;
    for (int i = 0; i < firstPageLines; i++) {
        chars += s_responseLines[i].length() + 1; // +1 approximates the space/line break
    }
    s_typewriterTargetChars = chars;
    s_typewriterStartMs = millis();
    s_currentPage = 0;
    s_pageStartMs = millis();
    s_paginating = false;
}

bool renderResponse(const String &fullText, unsigned long apiLatencyMs) {
    if (fullText != s_lastResponseText) {
        s_lastResponseText = fullText;
        resetResponseAnimationState(fullText);
    }

    oled.clearDisplay();

    // Header: AI sparkle icon + API latency, small and unobtrusive.
    oled.drawBitmap(0, 0, FayasDisplay::ICON_AI, ICON_SIZE, ICON_SIZE, SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(20, 4);
    char latencyStr[20];
    snprintf(latencyStr, sizeof(latencyStr), "%lums", apiLatencyMs);
    oled.print(latencyStr);
    oled.drawFastHLine(0, 16, SCREEN_WIDTH_LOCAL(), SSD1306_WHITE);

    bool done = false;

    if (!s_paginating) {
        // --- Typewriter phase: reveal the first page's characters ---
        unsigned long elapsed = millis() - s_typewriterStartMs;
        int charsToShow = (int)((elapsed * TYPEWRITER_CHARS_PER_SEC) / 1000UL);
        if (charsToShow > s_typewriterTargetChars) charsToShow = s_typewriterTargetChars;

        int remaining = charsToShow;
        int y = 20;
        int firstPageLines = min(LINES_PER_PAGE, s_responseLineCount);
        for (int i = 0; i < firstPageLines; i++) {
            String &line = s_responseLines[i];
            int show = min((int)line.length(), remaining);
            if (show > 0) {
                oled.setCursor(2, y);
                oled.print(line.substring(0, show));
            }
            remaining -= (line.length() + 1);
            if (remaining < 0) remaining = 0;
            y += 8;
        }

        if (charsToShow >= s_typewriterTargetChars) {
            s_paginating = true;
            s_pageStartMs = millis();
        }
    } else {
        // --- Pagination phase: show LINES_PER_PAGE lines at a time ---
        int totalPages = (s_responseLineCount + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
        if (totalPages < 1) totalPages = 1;

        unsigned long pageElapsed = millis() - s_pageStartMs;

        // Slide-up transition for the first few ms of each new page.
        int slideOffset = 0;
        if (pageElapsed < PAGE_TRANSITION_MS) {
            float t = pageElapsed / (float)PAGE_TRANSITION_MS;
            slideOffset = (int)((1.0f - t) * 10);
        }

        int startLine = s_currentPage * LINES_PER_PAGE;
        int endLine = min(startLine + LINES_PER_PAGE, s_responseLineCount);
        int y = 20 - slideOffset;
        for (int i = startLine; i < endLine; i++) {
            if (y >= CONTENT_TOP) {
                oled.setCursor(2, y);
                oled.print(s_responseLines[i]);
            }
            y += 8;
        }

        // Page indicator (e.g. "2/3") bottom-right when there are multiple pages.
        if (totalPages > 1) {
            char pageStr[10];
            snprintf(pageStr, sizeof(pageStr), "%d/%d", s_currentPage + 1, totalPages);
            oled.setCursor(SCREEN_WIDTH_LOCAL() - 24, 56);
            oled.print(pageStr);
        }

        if (pageElapsed >= PAGE_PAUSE_MS) {
            if (s_currentPage + 1 < totalPages) {
                s_currentPage++;
                s_pageStartMs = millis();
            } else {
                done = true;
            }
        }
    }

    oled.display();
    return done;
}

// Small local helper to avoid pulling in config.h purely for one constant
// already known to the display module.
static int SCREEN_WIDTH_LOCAL() { return 128; }

// ============================================================================
// ERROR SCREEN
// ============================================================================
void renderError(const char *message) {
    unsigned long elapsed = millis() - s_stateEnterMs;
    oled.clearDisplay();

    // Shake animation for the first 500ms to draw the eye, then settle.
    int shakeX = 0;
    if (elapsed < 500) {
        float t = elapsed / 500.0f;
        shakeX = (int)(sinf(t * 30.0f) * (1.0f - t) * 4.0f);
    }

    // Blinking warning icon (500ms on/off).
    if ((elapsed % 1000UL) < 600UL) {
        oled.drawBitmap(56 + shakeX, 10, FayasDisplay::ICON_WARNING, ICON_SIZE, ICON_SIZE, SSD1306_WHITE);
    }

    // Split the two-line message ("Title\nSubtitle") across two centered lines.
    String msg(message);
    int splitIdx = msg.indexOf('\n');
    String line1 = splitIdx >= 0 ? msg.substring(0, splitIdx) : msg;
    String line2 = splitIdx >= 0 ? msg.substring(splitIdx + 1) : "";

    oled.setTextSize(1);
    int line1X = 64 - (line1.length() * 3) + shakeX;
    oled.setCursor(max(0, line1X), 34);
    oled.print(line1);

    if (line2.length() > 0) {
        int line2X = 64 - (int)(line2.length() * 3) + shakeX;
        oled.setCursor(max(0, line2X), 46);
        oled.print(line2);
    }

    oled.display();
}

// ============================================================================
// SUCCESS SCREEN
// ============================================================================
static const unsigned long SUCCESS_DURATION_MS = 900;

bool renderSuccess() {
    unsigned long elapsed = millis() - s_stateEnterMs;
    oled.clearDisplay();

    float t = min(1.0f, elapsed / (float)SUCCESS_DURATION_MS);
    // Simple scale-in: draw the checkmark bitmap at full size once t
    // crosses a small threshold, with an expanding ring behind it for
    // visual "pop" (a true smooth bitmap scale isn't practical on this
    // display without a much larger sprite budget than a 16x16 glyph).
    int ringRadius = (int)(t * 20);
    oled.drawCircle(64, 28, ringRadius, SSD1306_WHITE);
    if (t > 0.15f) {
        oled.drawBitmap(56, 20, FayasDisplay::ICON_CHECK, ICON_SIZE, ICON_SIZE, SSD1306_WHITE);
    }

    oled.setTextSize(1);
    oled.setCursor(34, 46);
    oled.print("Done");

    oled.display();
    return elapsed >= SUCCESS_DURATION_MS;
}

} // namespace FayasAnimations
