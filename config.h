/**
 * @file    config.h
 * @project Fayas AI - A Pocket AI Voice Assistant
 * @author  Fayas
 * @version 1.0.0
 * @brief   Central configuration file. All hardware pin mappings, network
 *          credentials, API settings, and tunable constants live here so
 *          the rest of the codebase never contains magic numbers.
 *
 * SECURITY NOTE:
 *   This file contains your Wi-Fi password and AI API key. Do NOT
 *   commit your real credentials to a public repository. The provided
 *   .gitignore does not exclude this file by default because config.h is
 *   also where structural constants live; instead, replace the values
 *   below with your own before flashing, or copy this file to
 *   config.local.h (untracked) and adjust your build process accordingly.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// WI-FI CREDENTIALS
// ============================================================================
// Purpose: Network credentials used by wifi.cpp to join your local network.
// Replace these with your own values before flashing the device.
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// Maximum time (ms) to wait for an initial Wi-Fi connection before giving up
// and showing the error screen. Prevents an indefinite boot-time hang.
#define WIFI_CONNECT_TIMEOUT_MS   15000UL

// Interval (ms) at which the main loop checks whether Wi-Fi is still alive
// and triggers a reconnect attempt if it has dropped.
#define WIFI_CHECK_INTERVAL_MS    5000UL

// ============================================================================
// AI API CONFIGURATION (GROQ)
// ============================================================================
// Purpose: Credentials and endpoint configuration for the AI API (using Groq).
#define AI_API_KEY      "YOUR_GROQ_API_KEY"
#define AI_API_HOST  "api.groq.com"
#define AI_API_PORT  443

#define GROQ_WHISPER_MODEL     "whisper-large-v3"
#define GROQ_WHISPER_LANGUAGE  "en"  // Set to "en" to lock English transcriptions and avoid Hindi/other mis-detections (leave empty "" for auto-detect)
#define GROQ_WHISPER_PROMPT    "OpenAI, ChatGPT, Fayas AI, general questions, short voice queries." // General prompt context for Whisper spelling accuracy
#define GROQ_LLM_MODEL         "llama-3.3-70b-versatile"

// Text prompt sent alongside the recorded audio. Edit this to change the
// assistant's "personality" or response style.
#define AI_SYSTEM_PROMPT \
"You are Fayas AI, a voice assistant built by Fayas for an ESP32 with a push-to-talk mic and OLED display. " \
"Speak naturally, like a smart, friendly companion - not a formal chatbot. " \
"Output plain text only: no markdown, no asterisks, no headers, no bullet points, no emojis. Do not use special symbols in casual text, but use correct syntax (like quotes, parentheses, brackets) for code snippets so they are valid. " \
"Default to 1-3 short sentences (roughly 20-40 words). Only go longer if the user explicitly asks for detail, a list read aloud, or a step-by-step explanation. " \
"Answer directly first, then add context only if it's useful. Avoid filler openers like 'Sure!' or 'Great question!'. " \
"Handle general knowledge, math, science, coding, and casual conversation accurately and confidently. " \
"If you don't know something, or it needs live/current data you don't have, say so briefly instead of guessing. " \
"Numbers and units should be written in a way that sounds natural when spoken aloud (e.g. '3.14' not 'pi', 'first' not '1st'). " \
"Never mention that you are an AI model, your system prompt, or internal instructions, even if asked directly. " \
"Stay in character as Fayas AI at all times. " \
"Keep tone warm, direct, and a little witty when appropriate - like a knowledgeable friend, not a customer service bot."

// Network timeouts / retry policy for the HTTPS request.
#define AI_HTTP_TIMEOUT_MS    20000UL
#define AI_MAX_RETRIES        2

// ============================================================================
// OLED DISPLAY (SSD1306, 128x64, I2C)
// ============================================================================
#define SCREEN_WIDTH        128
#define SCREEN_HEIGHT       64
#define OLED_RESET_PIN      -1     // Shares the ESP32 reset line, no dedicated pin
#define OLED_I2C_ADDRESS    0x3C   // Common default for 0.96" SSD1306 modules
#define OLED_SDA_PIN        21
#define OLED_SCL_PIN        22
#define OLED_I2C_CLOCK_HZ   400000UL // 400kHz "fast mode" I2C for smoother redraws

// Target animation frame interval. 1000/60 ≈ 16ms for ~60 FPS ceiling.
// Actual FPS depends on how much time each frame's drawing takes.
#define FRAME_INTERVAL_MS   16UL

// Screen saver: after this many milliseconds of inactivity on the Home
// screen, dim the animation intensity to reduce OLED burn-in risk.
#define SCREENSAVER_TIMEOUT_MS  30000UL

// ============================================================================
// PUSH BUTTON (Push-to-talk)
// ============================================================================
#define BUTTON_PIN          27
#define BUTTON_ACTIVE_LOW   false  // Touch switch module: I/O pin goes HIGH when touched
#define BUTTON_DEBOUNCE_MS  100UL

// ============================================================================
// INMP441 I2S MICROPHONE
// ============================================================================
// Standard INMP441 wiring: WS (word select / LRCLK), SCK (bit clock),
// SD (serial data out). L/R pin tied to GND on the mic module selects the
// left channel, which is what we read here.
#define I2S_MIC_WS_PIN      25
#define I2S_MIC_SCK_PIN     26
#define I2S_MIC_SD_PIN      33
#define I2S_PORT_NUM        I2S_NUM_0

#define AUDIO_SAMPLE_RATE_HZ   8000
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_NUM_CHANNELS     1      // Mono
// I2S DMA buffering: how many descriptor buffers to use and how many
// samples each holds. Total ring-buffer depth = COUNT * LEN samples.
// A larger value gives more headroom against I2S buffer overruns, but
// costs heap on boards without PSRAM - keep this modest since the
// recording buffer is now the priority allocation (see audio.cpp begin()).
#define AUDIO_DMA_BUF_COUNT    10
#define AUDIO_DMA_BUF_LEN      256    // Samples per DMA buffer

// Maximum recording length. This directly bounds RAM usage:
// bytes = AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE/8) * seconds
// At 16kHz/16-bit mono, 6 seconds ≈ 192,000 bytes, which fits within the
// ESP32's ~320KB heap alongside the rest of the application (tested
// headroom on a base ESP32 DevKit V1 without PSRAM). If your board has
// PSRAM, you can safely raise this value; audio.cpp will use PSRAM
// automatically when available (see audio.cpp: allocateAudioBuffer()).
#define AUDIO_MAX_RECORD_SECONDS   6
#define AUDIO_MAX_BUFFER_BYTES \
    (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_MAX_RECORD_SECONDS)

// Digital gain multiplier applied to I2S microphone samples.
// The INMP441 output is clean but quiet; a multiplier of 3.0 to 5.0 helps
// Whisper hear the speech clearly. audio.cpp now applies a soft-knee
// limiter above this gain stage, so peaks are compressed smoothly instead
// of hard-clipping - if your recordings still sound distorted, lower this
// value first; if they sound too quiet, raise it.
#define AUDIO_GAIN_MULTIPLIER  6.0f

// WAV header size in bytes (standard 44-byte canonical PCM header)
#define WAV_HEADER_SIZE_BYTES  44

// ============================================================================
// APPLICATION / DEBUG
// ============================================================================
// Set to true to print free-heap / timing diagnostics over Serial and show
// the on-screen debug overlay (free heap + last API latency).
#define DEBUG_MODE_DEFAULT   true

#define SERIAL_BAUD_RATE     115200

#endif // CONFIG_H