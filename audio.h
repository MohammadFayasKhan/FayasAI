/**
 * @file    audio.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   INMP441 I2S microphone capture. Records 16kHz/16-bit mono PCM
 *          directly into a RAM buffer (PSRAM if available) and wraps it in
 *          a standard 44-byte WAV header, ready to hand to ai.cpp.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>

namespace FayasAudio {

/**
 * @brief Initialize the I2S peripheral for the INMP441 microphone and
 *        allocate the recording buffer.
 * @return true on success; false if buffer allocation failed (e.g. out
 *         of memory) or the I2S driver could not be installed.
 */
bool begin();

/**
 * @brief Begin a new recording session. Resets the internal write cursor.
 *        Call once when the user presses the push-to-talk button.
 */
void startRecording();

/**
 * @brief Pull any samples currently available from the I2S DMA buffers into
 *        the recording buffer. Must be called frequently (every loop()
 *        iteration) while recording is active so animations stay smooth
 *        instead of blocking on a single long i2s_read() call.
 *
 *        While pumping, this also runs a lightweight energy-based Voice
 *        Activity Detector (VAD): once speech has been detected, a long
 *        enough run of trailing silence auto-ends the turn. The very first
 *        AUDIO_WARMUP_MS of samples are discarded (mic power-on transient).
 *
 * @return true if recording should stop automatically — either the hard
 *         capacity cap was reached OR the VAD detected end-of-speech. The
 *         caller reacts identically to a button release in both cases.
 */
bool pump();

/**
 * @brief Whether the VAD has detected the start of speech in the current
 *        recording yet. Useful for the UI (e.g. "listening..." vs "speak
 *        now") and for deciding whether a clip is worth sending.
 */
bool speechDetected();

/**
 * @brief Reason the most recent recording ended, for logging/UX.
 */
enum class StopReason : uint8_t {
  NONE,          // still recording / not yet stopped
  BUTTON,        // user released the push-to-talk button
  VAD_SILENCE,   // VAD detected end of speech
  BUFFER_FULL    // hard capacity cap reached
};

/// The reason the last recording stopped.
StopReason lastStopReason();

/**
 * @brief Stop the current recording session. Call when the button is
 *        released. Safe to call even if the buffer auto-filled already.
 */
void stopRecording();

/// Elapsed recording time in milliseconds, for the on-screen timer.
unsigned long getElapsedMs();

/**
 * @brief Finalize the recording into a playable WAV byte buffer by writing
 *        the 44-byte canonical PCM header in front of the captured samples.
 * @param outSize Receives the total size (header + PCM data) in bytes.
 * @return Pointer to the internal WAV buffer (do not free; owned by this
 *         module), or nullptr if no recording has been made yet.
 */
const uint8_t *finalizeWav(size_t &outSize);

/// Free the audio recording buffer to reclaim memory for the network requests.
void releaseBuffer();

/// Number of raw PCM bytes captured in the most recent recording.
size_t getRecordedByteCount();

} // namespace FayasAudio

#endif // AUDIO_H
