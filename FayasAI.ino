/**
 * @file    Fayas_AI.ino
 * @project Fayas AI - A Pocket AI Voice Assistant
 * @author  Fayas
 * @version 1.0.0
 * @brief   Entry point and the top-level application state machine.
 *          Wires together wifi.h, display.h, animations.h, audio.h, and
 *          ai.h to implement the full user flow:
 *
 *          Boot -> Home -> (button) Listening -> Thinking -> Response ->
 *          Success -> Home, with Error reachable from any network step.
 *
 * @note Arduino requires the .ino file's name to match its containing
 *       folder (here, both are "Fayas_AI"). The project brief referred to
 *       this file as "main.ino"; it is functionally identical, just
 *       renamed to satisfy the Arduino IDE's build requirement so the
 *       sketch actually compiles when opened.
 */

#include "ai.h"
#include "animations.h"
#include "audio.h"
#include "config.h"
#include "conversation.h"
#include "display.h"
#include "fayaswifi.h"
#include "utils.h"
#include <esp_system.h>

// ============================================================================
// Global application state
// ============================================================================
static AppState g_state = AppState::BOOT;
static DebouncedButton g_button(BUTTON_PIN, BUTTON_ACTIVE_LOW,
                                BUTTON_DEBOUNCE_MS);
static NonBlockingTimer g_frameTimer(FRAME_INTERVAL_MS);
static bool g_debugMode = DEBUG_MODE_DEFAULT;
// Set false each time recording starts so the listening screen is drawn once
// on entry; the animation is otherwise suppressed during capture to avoid
// starving the I2S DMA consumer loop (see handleListening).
static bool g_listenScreenDrawn = false;

// Cached values refreshed periodically rather than every frame, since
// WiFi.RSSI()/status() calls are relatively expensive to call at 60Hz.
static NonBlockingTimer g_wifiPollTimer(1000);
static bool g_wifiConnectedCache = false;
static int g_signalBarsCache = 0;

// Result of the most recent AI call, held so the Response/Error
// screens can render it across multiple frames.
static String g_lastResponseText;
static unsigned long g_lastLatencyMs = 0;
static const char *g_lastErrorMessage = "Unknown Error";

/// Helper to get a human-readable name for each state (for Serial logging).
static const char *stateName(AppState s) {
  switch (s) {
  case AppState::BOOT:
    return "BOOT";
  case AppState::HOME:
    return "HOME";
  case AppState::LISTENING:
    return "LISTENING";
  case AppState::THINKING:
    return "THINKING";
  case AppState::RESPONSE:
    return "RESPONSE";
  case AppState::SUCCESS:
    return "SUCCESS";
  case AppState::ERROR_STATE:
    return "ERROR";
  case AppState::CONFIG_PORTAL:
    return "CONFIG_PORTAL";
  default:
    return "UNKNOWN";
  }
}

/// Central place to change state, so every transition also resets the
/// relevant animation state via FayasAnimations::onEnterState().
static void transitionTo(AppState newState) {
  Serial.printf("[State] %s -> %s\n", stateName(g_state), stateName(newState));
  g_state = newState;
  FayasAnimations::onEnterState(newState);
}

// ----------------------------------------------------------------------------
// setup()
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(
      200); // Brief pause for USB-serial enumeration only; not on the hot path.

  FayasWiFi::initCredentials();

  Serial.println(F("\n========================================"));
  Serial.println(F("  Fayas AI - Pocket AI Assistant"));
  Serial.println(F("========================================"));
  Serial.printf("[Boot] Free heap: %d bytes\n", ESP.getFreeHeap());

  // Print reset reason to diagnose crashes/restarts
  esp_reset_reason_t rstReason = esp_reset_reason();
  Serial.printf(
      "[Boot] Reset Reason Code: %d (%s)\n", rstReason,
      (rstReason == ESP_RST_BROWNOUT)
          ? "BROWNOUT RESET - Check USB cable or supply!"
      : (rstReason == ESP_RST_PANIC) ? "SOFTWARE PANIC/CRASH!"
      : (rstReason == ESP_RST_INT_WDT || rstReason == ESP_RST_TASK_WDT)
          ? "WATCHDOG RESET!"
          : "Normal Boot/Reset");

  // --- OLED ---
  Serial.println(F("[Boot] Initializing OLED display..."));
  if (!FayasDisplay::begin()) {
    Serial.println(F("[Boot] FATAL: OLED not detected! Check SDA=GPIO21, "
                     "SCL=GPIO22, I2C addr=0x3C"));
    while (true) {
      delay(1000);
    }
  }
  Serial.println(F("[Boot] OLED: OK"));

  // --- Animations & Button ---
  FayasAnimations::begin();
  g_button.begin();
  Serial.printf("[Boot] Button on GPIO %d (active-%s), debounce=%lums\n",
                BUTTON_PIN, BUTTON_ACTIVE_LOW ? "LOW" : "HIGH",
                BUTTON_DEBOUNCE_MS);

  // --- Audio / I2S ---
  Serial.println(F("[Boot] Initializing I2S microphone..."));
  if (!FayasAudio::begin()) {
    Serial.println(F("[Boot] FAILED: Audio init failed (I2S or memory). Check "
                     "INMP441 wiring."));
    g_lastErrorMessage = "Mic Init Failed\nCheck Wiring";
    while (true) {
      FayasAnimations::renderError(g_lastErrorMessage);
      delay(100);
    }
  }
  Serial.println(F("[Boot] Audio: OK"));
  Serial.printf("[Boot] Free heap after audio: %d bytes\n", ESP.getFreeHeap());

  // --- Conversation memory ---
  FayasConversation::begin();
  Serial.println(F("[Boot] Conversation memory ready"));

  // --- WiFi ---
  Serial.printf("[Boot] Connecting to WiFi: %s ...\n", FayasWiFi::getSSID().c_str());
  if (!FayasWiFi::connect()) {
    Serial.println(F("[Boot] FAILED: WiFi connect timed out. Launching Config AP FayasAI..."));
    FayasWiFi::startConfigPortal("Wi-Fi connection timed out. Please update credentials below.");
    transitionTo(AppState::CONFIG_PORTAL);
    return;
  }
  Serial.printf("[Boot] WiFi: OK  IP=%s  RSSI=%lddBm\n",
                FayasWiFi::getLocalIP().c_str(), FayasWiFi::getRSSI());

  Serial.println(F("[Boot] All systems ready. Starting boot animation."));
  Serial.println(F("========================================\n"));
  transitionTo(AppState::BOOT);
}

// ----------------------------------------------------------------------------
// State handlers
// ----------------------------------------------------------------------------

static void updateWifiCache() {
  if (g_wifiPollTimer.ready()) {
    g_wifiConnectedCache = FayasWiFi::isConnected();
    g_signalBarsCache = rssiToBars(FayasWiFi::getRSSI());
  }
}

static void handleBoot(bool frameDue) {
  if (frameDue) {
    if (FayasAnimations::renderBoot()) {
      Serial.println(
          F("[Home] Boot animation complete. Waiting for button press..."));
      transitionTo(AppState::HOME);
    }
  }
}

static void handleHome(bool frameDue) {
  if (frameDue) {
    FayasAnimations::renderHome(g_wifiConnectedCache, g_signalBarsCache,
                                g_debugMode);
  }

  if (g_button.wasPressed()) {
    Serial.println(F("[Home] >>> BUTTON PRESSED <<<"));
    if (!g_wifiConnectedCache) {
      Serial.println(F("[Home] WiFi not connected! Showing error."));
      g_lastErrorMessage = "WiFi Lost\nReconnect...";
      transitionTo(AppState::ERROR_STATE);
      return;
    }
    // Drop stale conversation context so a brand-new session doesn't inherit
    // unrelated history from a chat the user had a while ago.
    if (FayasConversation::expireIfStale()) {
      Serial.println(F("[Home] Conversation history expired (session idle)"));
    }

    Serial.println(F("[Listen] Starting recording..."));
    FayasAudio::startRecording();
    g_listenScreenDrawn = false; // redraw listening screen once on entry
    transitionTo(AppState::LISTENING);
  }
}

static void handleListening(bool frameDue) {
  // Animation is SUPPRESSED during capture. RAW-word diagnostics proved the
  // intermittent zero chunks come from I2S DMA starvation: a full SSD1306 I2C
  // blit blocks the CPU ~20-35 ms, longer than a DMA read interval, so the ring
  // overflows with zero frames during the blit. With the animation off, pump()
  // runs unobstructed every loop iteration and zeroHeavy stayed 0 across 78k+
  // reads with accurate transcription. We render the listening screen ONCE on
  // entry (via g_listenScreenDrawn, reset at startRecording) and otherwise
  // leave the display alone until recording ends.
  bool bufferFull = FayasAudio::pump();
  unsigned long elapsed = FayasAudio::getElapsedMs();

  if (!g_listenScreenDrawn) {
    FayasAnimations::renderListening(elapsed);
    g_listenScreenDrawn = true;
  }
  (void)frameDue; // animation intentionally suppressed during capture

  bool buttonReleased = g_button.wasReleased();
  if (buttonReleased || bufferFull) {
    // bufferFull already stopped recording inside pump() and set the stop
    // reason; a button release stops it here.
    if (buttonReleased && !bufferFull) {
      FayasAudio::stopRecording();
    }
    size_t recordedBytes = FayasAudio::getRecordedByteCount();

    switch (FayasAudio::lastStopReason()) {
    case FayasAudio::StopReason::VAD_SILENCE:
      Serial.println(F("[Listen] Auto-stopped: end of speech detected (VAD)"));
      break;
    case FayasAudio::StopReason::BUFFER_FULL:
      Serial.println(F("[Listen] Buffer full (max recording reached)"));
      break;
    default:
      Serial.println(F("[Listen] >>> BUTTON RELEASED <<<"));
      break;
    }
    Serial.printf("[Listen] Recorded %u bytes (%.1f KB)\n", recordedBytes,
                  recordedBytes / 1024.0f);

    if (recordedBytes == 0) {
      Serial.println(
          F("[Listen] No audio captured (tap too short). Returning home."));
      transitionTo(AppState::HOME);
      return;
    }
    transitionTo(AppState::THINKING);
  }
}

/**
 * @brief Progress callback handed to FayasAI::sendAudio(). It is
 *        invoked many times per second while the upload/response-wait is
 *        in progress; g_frameTimer keeps actual rendering capped to the
 *        configured animation frame rate so this stays cheap to call
 *        often. This is what keeps the Thinking screen animating for the
 *        entire duration of the (otherwise synchronous) network call,
 *        rather than freezing on a single frame.
 */
static void onAIProgress() {
  if (g_frameTimer.ready()) {
    FayasAnimations::renderThinking();
  }
}

static void handleThinking() {
  // Render the first frame immediately so the Thinking animation
  // appears the instant we enter this state, before any network I/O
  // has started.
  FayasAnimations::renderThinking();

  size_t wavSize = 0;
  const uint8_t *wavData = FayasAudio::finalizeWav(wavSize);

  if (wavData == nullptr) {
    Serial.println(F("[Think] ERROR: finalizeWav returned null!"));
    FayasAudio::releaseBuffer(); // Free the buffer to prevent memory leaks on
                                 // rejection
    g_lastErrorMessage = "No Audio\nTry Again";
    transitionTo(AppState::ERROR_STATE);
    return;
  }
  Serial.printf("[Think] WAV ready: %u bytes (%.1f KB). Sending to AI...\n",
                wavSize, wavSize / 1024.0f);
  Serial.printf("[Think] Free heap before API call: %u bytes\n",
                ESP.getFreeHeap());
  Serial.printf("[Think] Largest free block: %u bytes\n",
                ESP.getMaxAllocHeap());

  String responseText;
  String transcript;
  unsigned long latencyMs = 0;

  // Retry transient failures (connect/timeout) a bounded number of times
  // before surfacing an error, so a single dropped packet or momentary
  // hiccup doesn't send the user straight to the error screen. Note the WAV
  // buffer is freed inside sendAudio() after the first upload attempt, so a
  // retry can only happen for failures that occur BEFORE the body is sent
  // (connect failures). Post-upload failures fall through to the error path,
  // which matches the buffer's lifetime.
  FayasAI::AIResult result = FayasAI::AIResult::ERR_CONNECT_FAILED;
  for (uint8_t attempt = 0; attempt <= AI_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      Serial.printf("[Think] Retry attempt %u/%u...\n", attempt, AI_MAX_RETRIES);
    }
    result = FayasAI::sendAudio(wavData, wavSize, responseText, transcript,
                                latencyMs, onAIProgress);
    // Only retry connection failures — the one class of error where a fresh
    // attempt is both safe (buffer still valid) and likely to help.
    if (result == FayasAI::AIResult::OK ||
        result != FayasAI::AIResult::ERR_CONNECT_FAILED) {
      break;
    }
  }

  // Safety net: sendAudio() already frees the buffer after the WAV upload
  // body is sent, but call releaseBuffer() here too in case an early
  // return skipped the internal free (e.g. connect failure before upload).
  FayasAudio::releaseBuffer();

  g_lastLatencyMs = latencyMs;
  Serial.printf("[Think] API call complete. Latency: %lums\n", latencyMs);

  if (result != FayasAI::AIResult::OK) {
    Serial.printf("[Think] API ERROR: %s\n", FayasAI::resultToMessage(result));
    g_lastErrorMessage = FayasAI::resultToMessage(result);
    transitionTo(AppState::ERROR_STATE);
    return;
  }

  Serial.println(F("[Think] SUCCESS! Response received:"));
  Serial.println("--------------------");
  Serial.println(responseText);
  Serial.println("--------------------");

  // Record this exchange so the next question has conversational context.
  if (transcript.length() > 0) {
    FayasConversation::addTurn(transcript, responseText);
    Serial.printf("[Think] Stored turn. History now holds %u exchange(s)\n",
                  (unsigned)FayasConversation::count());
  }

  g_lastResponseText = responseText;
  transitionTo(AppState::RESPONSE);
}

static void handleResponse(bool frameDue) {
  if (frameDue) {
    FayasAnimations::renderResponse(g_lastResponseText, g_lastLatencyMs);
  }

  if (g_button.wasPressed()) {
    Serial.println(F("[Response] Button pressed. Transitioning to Success."));
    transitionTo(AppState::SUCCESS);
  }
}

static void handleError(bool frameDue) {
  if (frameDue) {
    FayasAnimations::renderError(g_lastErrorMessage);
  }

  // Allow a button press to retry: if WiFi is the underlying problem,
  // attempt a reconnect; otherwise just return to Home so the user can
  // try recording again.
  if (g_button.wasPressed()) {
    Serial.println(F("[Error] Button pressed on error screen. Retrying..."));
    if (!g_wifiConnectedCache) {
      Serial.println(F("[Error] Attempting WiFi reconnect..."));
      FayasWiFi::reconnectIfNeeded();
    }
    transitionTo(AppState::HOME);
  }
}

static void handleSuccess(bool frameDue) {
  if (frameDue) {
    if (FayasAnimations::renderSuccess()) {
      transitionTo(AppState::HOME);
    }
  }
}

// ----------------------------------------------------------------------------
// loop()
// ----------------------------------------------------------------------------
void loop() {
  g_button.update();
  updateWifiCache();

  // Background Wi-Fi watchdog: if we're sitting in the Error screen due
  // to a WiFi drop, this will quietly bring us back once the network
  // recovers (the user still sees the Error screen with a chance to
  // retry manually in the meantime).
  if (g_state != AppState::LISTENING && g_state != AppState::THINKING && g_state != AppState::CONFIG_PORTAL) {
    FayasWiFi::reconnectIfNeeded();
  }

  // Cap the animation frame rate for consistent motion speed across
  // screens; audio pumping in handleListening() still runs every loop
  // iteration regardless of this cap, since it doesn't touch the display.
  bool frameDue = g_frameTimer.ready();

  switch (g_state) {
  case AppState::BOOT:
    handleBoot(frameDue);
    break;
  case AppState::HOME:
    handleHome(frameDue);
    break;
  case AppState::LISTENING:
    handleListening(frameDue);
    break;
  case AppState::THINKING:
    // handleThinking() performs a blocking network call internally;
    // it manages its own single render call before blocking.
    handleThinking();
    break;
  case AppState::RESPONSE:
    handleResponse(frameDue);
    break;
  case AppState::SUCCESS:
    handleSuccess(frameDue);
    break;
  case AppState::ERROR_STATE:
    handleError(frameDue);
    break;
  case AppState::CONFIG_PORTAL:
    FayasWiFi::handlePortal();
    if (frameDue) {
      FayasAnimations::renderConfigPortal("FayasAI", "fayasai.local");
    }
    break;
  }
}
