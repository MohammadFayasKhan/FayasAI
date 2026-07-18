/**
 * @file    audio.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of I2S microphone capture and WAV framing.
 *
 * NOTE: This file uses the NEW ESP-IDF 5.x I2S driver API
 *       (driver/i2s_std.h) instead of the legacy driver/i2s.h API.
 *       The legacy API (i2s_driver_install, i2s_set_pin, i2s_read)
 *       is deprecated in ESP32 Arduino Core 3.x and may fail at
 *       runtime even though it still compiles.
 */

#include "audio.h"
#include "config.h"
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace FayasAudio {

// Handle to the I2S RX (receive/microphone) channel, obtained from
// i2s_new_channel() and used for all subsequent read operations.
static i2s_chan_handle_t s_rxHandle = nullptr;

// The buffer holds room for the 44-byte WAV header followed by up to
// AUDIO_MAX_BUFFER_BYTES of raw PCM. The header is written once at the end
// (finalizeWav), so during recording we only ever write into the PCM
// region starting at WAV_HEADER_SIZE_BYTES.
static uint8_t *s_buffer = nullptr;
static size_t s_bufferCapacity =
    0; // WAV_HEADER_SIZE_BYTES + AUDIO_MAX_BUFFER_BYTES
static size_t s_pcmBytesWritten = 0; // Bytes of PCM data captured so far
static bool s_recording = false;
static unsigned long s_recordStartMs = 0;

// DC Blocking (High-pass) filter state
static float s_lastRaw = 0.0f;
static float s_lastFiltered = 0.0f;
// Diagnostics state
static uint32_t s_numClippedSamples = 0;
static uint32_t s_numZeroSamples = 0;
static double s_sumSquares = 0;
static int16_t s_peakAmplitude = 0;
static uint32_t s_totalSamplesProcessed = 0;

// Small scratch buffer used to pull samples out of the I2S DMA ring buffer
// on each pump() call, sized to move a reasonable chunk without blocking
// for long.
static const size_t I2S_READ_CHUNK_BYTES = 1024;
static uint8_t s_readChunk[I2S_READ_CHUNK_BYTES];

/// Track whether the board has PSRAM so we consistently use the right
/// allocator without re-probing every time.
static bool s_hasPSRAM = false;

/// The actual usable PCM capacity for the current board (bytes). On no-PSRAM
/// boards this is clamped to AUDIO_MAX_BUFFER_BYTES_NOPSRAM so the recording
/// buffer never grows past what internal SRAM can hold; on PSRAM boards it is
/// the full AUDIO_MAX_BUFFER_BYTES. Set in begin().
static size_t s_pcmCapacityBytes = 0;

// --- Voice Activity Detection (VAD) state ---
// Rolling accumulator for the current 20 ms analysis frame.
static double s_vadFrameSumSquares = 0.0;
static uint32_t s_vadFrameSamples = 0;
static bool s_speechDetected = false;      // has speech started this recording?
static unsigned long s_lastVoiceMs = 0;    // millis of the last speech frame
static uint32_t s_warmupSamplesRemaining = 0; // samples still to discard at start
static StopReason s_lastStopReason = StopReason::NONE;

// Number of raw PCM samples that make up one VAD analysis frame.
static const uint32_t VAD_FRAME_SAMPLES =
    (AUDIO_SAMPLE_RATE_HZ * VAD_FRAME_MS) / 1000;

/**
 * @brief Allocate the recording buffer, preferring PSRAM if the board has
 *        it so longer recordings don't compete with the rest of the
 *        application for the limited internal ~320KB heap.
 * @param sizeBytes Total bytes to allocate (header + max PCM payload).
 * @return Pointer to allocated memory, or nullptr on failure.
 */
static uint8_t *allocateAudioBuffer(size_t sizeBytes) {
  if (s_hasPSRAM) {
    uint8_t *psramBuf = (uint8_t *)ps_malloc(sizeBytes);
    if (psramBuf != nullptr) {
      return psramBuf;
    }
    // Fall through to internal heap if PSRAM allocation somehow fails.
  }
  return (uint8_t *)malloc(sizeBytes);
}

/// Print a snapshot of heap health to Serial for debugging memory issues.
static void printHeapDiag(const char *label) {
  Serial.printf("[Heap @ %s] Free: %u  MaxBlock: %u  PSRAM: %u\n",
                label,
                ESP.getFreeHeap(),
                ESP.getMaxAllocHeap(),
                ESP.getFreePsram());
}

bool begin() {
  // Purpose: Configure the I2S peripheral for INMP441 input.
  // Inputs: none (all parameters come from config.h).
  // Outputs: true if the I2S driver initialized successfully.
  // Logic: Uses the new ESP-IDF 5.x I2S standard-mode driver API.
  //
  // NOTE on buffer strategy: On boards WITHOUT PSRAM the audio buffer
  // and the TLS session cannot coexist in the ~320 KB internal SRAM.
  // Instead of allocating the buffer permanently, we allocate it fresh
  // in startRecording() and free it in releaseBuffer() right after the
  // WAV body is uploaded. This way audio and TLS take turns using the
  // same memory. On boards WITH PSRAM the buffer goes to external RAM
  // and this concern doesn't apply, but the pattern works fine either
  // way.

  s_hasPSRAM = psramFound();

  // Pick the PCM capacity for this board. PSRAM boards can afford the full
  // (long) ceiling in external RAM; no-PSRAM boards are clamped so the buffer
  // — which is freed before the TLS handshake — never exceeds what internal
  // SRAM can hold contiguously. VAD auto-stop means recordings normally end
  // long before either ceiling.
  s_pcmCapacityBytes =
      s_hasPSRAM ? (size_t)AUDIO_MAX_BUFFER_BYTES
                 : (size_t)AUDIO_MAX_BUFFER_BYTES_NOPSRAM;
  s_bufferCapacity = WAV_HEADER_SIZE_BYTES + s_pcmCapacityBytes;

  printHeapDiag("audio.begin() entry");
  Serial.printf("[Audio] PSRAM detected: %s\n", s_hasPSRAM ? "YES" : "NO");
  Serial.printf("[Audio] Max recording: %d s (%u PCM bytes)\n",
                s_hasPSRAM ? AUDIO_MAX_RECORD_SECONDS_PSRAM
                           : AUDIO_MAX_RECORD_SECONDS_NOPSRAM,
                (unsigned)s_pcmCapacityBytes);
  Serial.printf("[Audio] Recording buffer capacity: %u bytes (allocated on demand)\n",
                (unsigned)s_bufferCapacity);
  Serial.printf("[Audio] VAD: %s (speech RMS >%.0f, silence hangover %d ms)\n",
                VAD_ENABLED ? "ON" : "OFF",
                (float)VAD_SPEECH_RMS_THRESHOLD, VAD_SILENCE_HANGOVER_MS);

  // --- Step 1: Create the RX channel ---
  i2s_chan_config_t chanCfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
  chanCfg.dma_frame_num = AUDIO_DMA_BUF_LEN;

  if (i2s_new_channel(&chanCfg, NULL, &s_rxHandle) != ESP_OK) {
    Serial.println(F("[Audio] i2s_new_channel failed"));
    return false;
  }
  Serial.println(F("[Audio] I2S RX channel allocated"));

  // --- Step 2: Configure standard mode ---
  i2s_std_config_t stdCfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
          AUDIO_SAMPLE_RATE_HZ), // Set clock dynamically from config.h
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)I2S_MIC_SCK_PIN,
              .ws = (gpio_num_t)I2S_MIC_WS_PIN,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)I2S_MIC_SD_PIN,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  // Force 32-bit slot width so the INMP441 gets 64 SCK cycles per WS frame
  stdCfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  if (i2s_channel_init_std_mode(s_rxHandle, &stdCfg) != ESP_OK) {
    Serial.println(F("[Audio] i2s_channel_init_std_mode failed"));
    return false;
  }
  Serial.println(F("[Audio] I2S mode configured"));

  // --- Step 3: Enable the channel ---
  if (i2s_channel_enable(s_rxHandle) != ESP_OK) {
    Serial.println(F("[Audio] i2s_channel_enable failed"));
    return false;
  }
  Serial.println(F("[Audio] I2S channel enabled"));

  Serial.println(F("[Audio] I2S microphone initialized successfully"));
  printHeapDiag("audio.begin() exit");

  return true;
}

static unsigned long s_recordDurationMs = 0;

void startRecording() {
  s_pcmBytesWritten = 0;

  // Allocate the recording buffer on demand if it was freed after the
  // previous upload cycle (see releaseBuffer()). On no-PSRAM boards
  // this is the moment TLS buffers have already been freed, so a large
  // contiguous block is available.
  if (s_buffer == nullptr) {
    printHeapDiag("startRecording() pre-alloc");

    const size_t bytesPerSec =
        (size_t)AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8);

    if (s_hasPSRAM) {
      // PSRAM boards: the buffer lives in external RAM and never competes with
      // the TLS session in internal SRAM, so just allocate the full ceiling.
      size_t wantTotal = WAV_HEADER_SIZE_BYTES + s_pcmCapacityBytes;
      s_buffer = allocateAudioBuffer(wantTotal);
      if (s_buffer != nullptr) {
        s_bufferCapacity = wantTotal;
        Serial.printf("[Audio] Allocated PSRAM buffer: %u bytes (%.1f s max)\n",
                      (unsigned)wantTotal,
                      s_pcmCapacityBytes / (float)bytesPerSec);
      }
    } else {
      // ----------------------------------------------------------------------
      // No-PSRAM boards: the recording buffer and the TLS session must share
      // internal SRAM during the HTTPS upload, and mbedTLS needs a large
      // CONTIGUOUS block. It is NOT enough for malloc() to succeed — we must
      // also leave AUDIO_TLS_HEAP_RESERVE_BYTES of contiguous heap free
      // afterwards, or the later TLS handshake fails with "SSL - Memory
      // allocation failed" (the exact failure seen in the field).
      //
      // So: start from the ceiling and shrink one second at a time. After each
      // successful allocation, check the LARGEST remaining free block. Accept
      // the first size that both allocates AND leaves the TLS reserve intact.
      // Never shrink below the proven-good floor (AUDIO_MIN_RECORD_SECONDS).
      // ----------------------------------------------------------------------
      const int ceilSec = AUDIO_MAX_RECORD_SECONDS_NOPSRAM;
      const int floorSec = AUDIO_MIN_RECORD_SECONDS_NOPSRAM;

      for (int sec = ceilSec; sec >= floorSec; sec--) {
        size_t wantPcm = bytesPerSec * (size_t)sec;
        size_t wantTotal = WAV_HEADER_SIZE_BYTES + wantPcm;

        uint8_t *candidate = (uint8_t *)malloc(wantTotal);
        if (candidate == nullptr) {
          Serial.printf("[Audio] %ds buffer (%u B) alloc failed, trying smaller\n",
                        sec, (unsigned)wantTotal);
          continue;
        }

        size_t contiguousAfter = ESP.getMaxAllocHeap();
        bool tlsRoomOk = contiguousAfter >= AUDIO_TLS_HEAP_RESERVE_BYTES;

        // At the floor we keep the buffer even if the reserve isn't fully met:
        // this matches the original firmware's behaviour and is the best we
        // can do. Above the floor, reject sizes that would starve TLS.
        if (tlsRoomOk || sec == floorSec) {
          s_buffer = candidate;
          s_pcmCapacityBytes = wantPcm;
          s_bufferCapacity = wantTotal;
          Serial.printf(
              "[Audio] Allocated recording buffer: %u bytes (%ds max); "
              "contiguous heap left for TLS: %u bytes%s\n",
              (unsigned)wantTotal, sec, (unsigned)contiguousAfter,
              tlsRoomOk ? "" : " (BELOW reserve - floor size)");
          break;
        }

        // Allocated fine but too little contiguous heap left for TLS: free and
        // try a smaller buffer so the handshake won't fail later.
        Serial.printf(
            "[Audio] %ds buffer leaves only %u B contiguous (< %u reserve); "
            "shrinking\n",
            sec, (unsigned)contiguousAfter,
            (unsigned)AUDIO_TLS_HEAP_RESERVE_BYTES);
        free(candidate);
      }
    }

    if (s_buffer == nullptr) {
      Serial.println(F("[Audio] ERROR: could not allocate recording buffer!"));
      printHeapDiag("startRecording() alloc FAILED");
      s_recording = false;
      return;
    }
    printHeapDiag("startRecording() post-alloc");
  }

  // Wait 100ms with BCLK running to give the INMP441 power supply and digital
  // filters ample time to boot up and output stable audio data.
  delay(100);

  // Drain any startup noise and stale samples from the DMA buffer
  size_t bytesRead = 0;
  uint8_t tempBuf[256];
  while (i2s_channel_read(s_rxHandle, tempBuf, sizeof(tempBuf), &bytesRead,
                          0) == ESP_OK &&
         bytesRead > 0) {
    // Just discard startup noise
  }

  s_recording = true;
  s_recordStartMs = millis();
  s_recordDurationMs = 0;
  s_lastRaw = 0.0f;
  s_lastFiltered = 0.0f;
  s_numClippedSamples = 0;
  s_numZeroSamples = 0;
  s_sumSquares = 0;
  s_peakAmplitude = 0;
  s_totalSamplesProcessed = 0;

  // Reset VAD / turn-detection state for the new utterance.
  s_vadFrameSumSquares = 0.0;
  s_vadFrameSamples = 0;
  s_speechDetected = false;
  s_lastVoiceMs = s_recordStartMs;
  s_lastStopReason = StopReason::NONE;
  // Discard the first AUDIO_WARMUP_MS of samples (mic power-on transient). The
  // startRecording() drain above removes the DMA backlog; this additionally
  // skips the settling window from live capture so it never reaches the buffer
  // or the VAD analyser.
  s_warmupSamplesRemaining =
      (uint32_t)((AUDIO_SAMPLE_RATE_HZ * AUDIO_WARMUP_MS) / 1000);
}

bool pump() {
  // Purpose: Non-blocking drain of the I2S DMA buffer into our recording
  //          buffer. Called every loop() iteration while recording.
  // Logic: Since the I2S peripheral is configured for 32-bit data bit width
  //        and 32-bit slot width, the hardware remains perfectly synchronized
  //        with the 64-bit I2S frame (32 bits per channel). We read 32-bit signed
  //        integers, and right-shift them by 16 bits in software to get the clean
  //        16-bit range data.
  if (!s_recording || s_buffer == nullptr) {
    return false;
  }

  size_t remainingCapacity = s_pcmCapacityBytes - s_pcmBytesWritten;
  if (remainingCapacity == 0) {
    s_lastStopReason = StopReason::BUFFER_FULL;
    stopRecording();
    return true; // Buffer full - caller should stop recording.
  }

  // We run I2S hardware in mono mode.
  // 1 frame contains 1 sample (32-bit/4 bytes).
  // Since we write 1 mono sample (2 bytes) per I2S frame, the ratio of
  // input-to-output bytes is 2 to 1.
  size_t bytesToRequest = min(I2S_READ_CHUNK_BYTES, remainingCapacity * 2);
  // Align to 4 bytes (one 32-bit frame)
  bytesToRequest = (bytesToRequest / 4) * 4;

  if (bytesToRequest == 0) {
    return false;
  }

  size_t bytesRead = 0;
  esp_err_t result =
      i2s_channel_read(s_rxHandle, s_readChunk, bytesToRequest, &bytesRead,
                       0 /* timeout_ms = don't block */);

  // ---------------------------------------------------------------------
  // DIAGNOSTIC INSTRUMENTATION (temporary — capture behavior unchanged).
  // Classifies WHERE zero samples originate. We scan the RAW 32-bit words
  // straight from the driver, BEFORE any shift/DC-filter/gain, and count how
  // many are exact zero. Combined with the driver's result code and bytesRead,
  // this distinguishes:
  //   * zeros already present in the DMA buffer  -> upstream (driver/DMA/mic)
  //   * raw words non-zero but output zero        -> our DSP/pointer math
  // Logging is throttled and only dumps hex when a zero-heavy chunk appears,
  // so it does not itself starve the capture loop.
  {
    static uint32_t s_diagReadCounter = 0;
    static uint32_t s_diagZeroReadTotal = 0;
    static uint32_t s_diagOkEmpty = 0;
    static uint32_t s_diagErr = 0;

    s_diagReadCounter++;
    if (result != ESP_OK) {
      s_diagErr++;
    } else if (bytesRead == 0) {
      s_diagOkEmpty++;
    }

    if (result == ESP_OK && bytesRead > 0) {
      const int32_t *rawWords = (const int32_t *)s_readChunk;
      size_t rawCount = bytesRead / 4;
      size_t rawZeros = 0;
      for (size_t k = 0; k < rawCount; k++) {
        if (rawWords[k] == 0) rawZeros++;
      }

      bool zeroHeavy = (rawZeros * 100) >= (rawCount * 50); // >=50% raw zeros
      // Log first 3 reads always (startup picture), then only zero-heavy ones,
      // throttled to at most 1 in 4 to avoid flooding the serial line.
      if (s_diagReadCounter <= 3 ||
          (zeroHeavy && (s_diagReadCounter % 4 == 0))) {
        Serial.printf(
            "[RAW] read#%lu result=%d bytesRead=%u rawWords=%u rawZeros=%u "
            "(%.0f%%) | hex: %08lx %08lx %08lx %08lx\n",
            (unsigned long)s_diagReadCounter, (int)result, (unsigned)bytesRead,
            (unsigned)rawCount, (unsigned)rawZeros,
            rawCount ? (100.0 * rawZeros / rawCount) : 0.0,
            (unsigned long)rawWords[0],
            rawCount > 1 ? (unsigned long)rawWords[1] : 0UL,
            rawCount > 2 ? (unsigned long)rawWords[2] : 0UL,
            rawCount > 3 ? (unsigned long)rawWords[3] : 0UL);
      }
      if (zeroHeavy) s_diagZeroReadTotal++;
    }

    // Periodic summary of read outcomes so we can see starvation vs errors.
    if (s_diagReadCounter % 64 == 0) {
      Serial.printf(
          "[RAW SUMMARY] reads=%lu zeroHeavy=%lu okEmpty(bytesRead==0)=%lu "
          "errors=%lu\n",
          (unsigned long)s_diagReadCounter,
          (unsigned long)s_diagZeroReadTotal, (unsigned long)s_diagOkEmpty,
          (unsigned long)s_diagErr);
    }
  }

  if (result == ESP_OK && bytesRead > 0) {
    size_t samplesRead = bytesRead / 4;
    int32_t *samples32 = (int32_t *)s_readChunk;

    int16_t *dest16 =
        (int16_t *)(s_buffer + WAV_HEADER_SIZE_BYTES + s_pcmBytesWritten);
    size_t samplesWritten = 0;

    int16_t peakMin = 32767;
    int16_t peakMax = -32768;

    // DC-blocking high-pass coefficient. The INMP441 has a small DC bias and
    // low-frequency rumble; a one-pole high-pass removes it so it doesn't eat
    // headroom or bias the VAD. This is the ONE piece of DSP a production mic
    // path needs; everything else (limiter, multi-stage gain) was removed
    // because it distorted real speech and triggered Whisper's " Thank you."
    // hallucination. R tuned to ~80 Hz cutoff at the capture rate.
#if AUDIO_SAMPLE_RATE_HZ == 8000
    const float R = 0.94f;
#else
    const float R = 0.97f; // ~80 Hz cutoff at 16 kHz
#endif

    // Loop through the mono read samples.
    //
    // Conversion model, matching xiaozhi-esp32's NoAudioCodec::Read()
    // (main/audio/codecs/no_audio_codec.cc): take the left-justified 24-bit
    // sample from the 32-bit I2S word, scale to int16, and SATURATE (clamp)
    // rather than compress. xiaozhi bakes ~+12 dB of gain into a `>>12` shift;
    // we keep the `>>16` MSB alignment and apply the same effective boost via
    // AUDIO_GAIN_MULTIPLIER, so gain is tunable in one place. No soft-knee
    // limiter: clamping preserves the waveform shape Whisper needs.
    for (size_t i = 0; i < samplesRead; i++) {
      // 24 valid bits are left-justified in [31:8]; >>16 takes the top 16.
      float raw = (float)(samples32[i] >> 16);

      // One-pole DC-blocking high-pass filter.
      float filtered = raw - s_lastRaw + R * s_lastFiltered;
      s_lastRaw = raw;
      s_lastFiltered = filtered;

      // Single tunable gain, then saturating clamp (xiaozhi-style).
      float scaled = filtered * AUDIO_GAIN_MULTIPLIER;

      int32_t val = (int32_t)scaled;
      bool clipped = false;
      if (val > 32767) {
        val = 32767;
        clipped = true;
      } else if (val < -32768) {
        val = -32768;
        clipped = true;
      }

      int16_t converted = (int16_t)val;

      // --- Mic warm-up: discard the settling window ---
      // We still run the DC-blocking filter above (so its state is warm by
      // the time real audio starts) but we neither store these samples nor
      // feed them to VAD/diagnostics.
      if (s_warmupSamplesRemaining > 0) {
        s_warmupSamplesRemaining--;
        continue;
      }

      dest16[samplesWritten] = converted;
      samplesWritten++;

      if (converted < peakMin) peakMin = converted;
      if (converted > peakMax) peakMax = converted;

      // Diagnostics update
      if (clipped) s_numClippedSamples++;
      if (converted == 0) s_numZeroSamples++;
      int16_t absConverted = (converted < 0) ? -converted : converted;
      if (absConverted > s_peakAmplitude) s_peakAmplitude = absConverted;
      s_sumSquares += ((double)converted * (double)converted);
      s_totalSamplesProcessed++;

      // --- Voice Activity Detection ---
      // Accumulate energy over a VAD_FRAME_MS window; when the frame is full,
      // compare its RMS against the speech threshold to update speech/silence
      // state. This costs one multiply-add per sample plus a sqrt per frame.
      s_vadFrameSumSquares += ((double)converted * (double)converted);
      s_vadFrameSamples++;
      if (s_vadFrameSamples >= VAD_FRAME_SAMPLES) {
        double frameRms = sqrt(s_vadFrameSumSquares / s_vadFrameSamples);
        if (frameRms >= (double)VAD_SPEECH_RMS_THRESHOLD) {
          s_speechDetected = true;
          s_lastVoiceMs = millis();
        }
        s_vadFrameSumSquares = 0.0;
        s_vadFrameSamples = 0;
      }
    }

    // Print peak values every ~10 chunks
    static int s_logCounter = 0;
    if (s_logCounter++ % 5 == 0 && samplesWritten > 0) {
      Serial.printf("[Audio Pump] Captured chunk peak: Min=%d, Max=%d\n",
                    peakMin, peakMax);
    }

    s_pcmBytesWritten += (samplesWritten * 2); // 2 bytes written per sample
  }

  // Hard capacity cap (safety net).
  if (s_pcmBytesWritten >= s_pcmCapacityBytes) {
    s_lastStopReason = StopReason::BUFFER_FULL;
    stopRecording();
    return true;
  }

#if VAD_ENABLED
  // VAD auto-stop: once speech has started, end the turn after a long enough
  // run of silence. A start-grace window keeps a slow starter from being cut
  // off before they've said anything.
  unsigned long now = millis();
  bool pastGrace = (now - s_recordStartMs) >= (unsigned long)VAD_START_GRACE_MS;
  if (s_speechDetected && pastGrace &&
      (now - s_lastVoiceMs) >= (unsigned long)VAD_SILENCE_HANGOVER_MS) {
    Serial.printf("[Audio VAD] End of speech: %lums silence after speech\n",
                  now - s_lastVoiceMs);
    s_lastStopReason = StopReason::VAD_SILENCE;
    stopRecording();
    return true;
  }
#endif

  return false;
}

bool speechDetected() { return s_speechDetected; }

StopReason lastStopReason() { return s_lastStopReason; }

void stopRecording() {
  if (s_recording) {
    s_recordDurationMs = millis() - s_recordStartMs;
    s_recording = false;
  }
}

unsigned long getElapsedMs() {
  if (s_recording) {
    return millis() - s_recordStartMs;
  }
  return s_recordDurationMs;
}

size_t getRecordedByteCount() { return s_pcmBytesWritten; }

/**
 * @brief Write a 32-bit little-endian value into a buffer at a given offset.
 *        WAV headers are little-endian regardless of host byte order, so
 *        we build them explicitly rather than relying on struct packing.
 */
static void writeLE32(uint8_t *buf, size_t offset, uint32_t value) {
  buf[offset + 0] = (uint8_t)(value & 0xFF);
  buf[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
  buf[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
  buf[offset + 3] = (uint8_t)((value >> 24) & 0xFF);
}

static void writeLE16(uint8_t *buf, size_t offset, uint16_t value) {
  buf[offset + 0] = (uint8_t)(value & 0xFF);
  buf[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
}

const uint8_t *finalizeWav(size_t &outSize) {
  // Purpose: Prepend a canonical 44-byte PCM WAV header to the captured
  //          samples so the buffer is a complete, valid .wav file that
  //          the AI API endpoint can parse.
  // Inputs: none (uses s_pcmBytesWritten captured during recording).
  // Outputs: outSize is set to header + PCM length; returns the buffer
  //          pointer, or nullptr if nothing has been recorded.
  // Logic: Standard RIFF/WAVE/fmt/data chunk layout for uncompressed PCM.
  // Possible errors: returns nullptr if begin() was never called
  //        successfully or no recording has happened yet.
  if (s_buffer == nullptr || s_pcmBytesWritten == 0) {
    outSize = 0;
    return nullptr;
  }

  // Trim trailing click/pop noise from the touch-release, but adaptively:
  // if the user is still speaking right up to the moment they release the
  // button (e.g. a short final word like "meta"), a blind fixed-length
  // trim would cut off real speech, not just the click. Instead, scan
  // backward from the end (within a bounded window) for the last sample
  // that's actually loud, and only trim from just after that point - so
  // genuine trailing silence + the click get removed, but real audio
  // right up to the edge is preserved. A small minimum trim is still
  // always applied since the click itself sits right at the boundary.
  const size_t maxTrimBytes =
      (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * 150) / 1000;
  const size_t minTrimBytes =
      (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * 30) / 1000;
  const int16_t SILENCE_AMPLITUDE =
      600; // below this is treated as "quiet enough to cut"

  if (s_pcmBytesWritten > maxTrimBytes) {
    int16_t *pcmSamples = (int16_t *)(s_buffer + WAV_HEADER_SIZE_BYTES);
    size_t totalSamples = s_pcmBytesWritten / 2;
    size_t windowSamples = maxTrimBytes / 2;
    size_t scanStart = totalSamples - windowSamples;

    // Find the last sample (within the trailing window) louder than
    // the silence threshold; keepUpToSample marks where real content
    // ends.
    size_t keepUpToSample = scanStart; // default: whole window is quiet
    for (size_t i = scanStart; i < totalSamples; i++) {
      int16_t s = pcmSamples[i];
      if (s > SILENCE_AMPLITUDE || s < -SILENCE_AMPLITUDE) {
        keepUpToSample = i + 1;
      }
    }

    size_t trimSamples = totalSamples - keepUpToSample;
    size_t minTrimSamples = minTrimBytes / 2;
    size_t maxTrimSamples = maxTrimBytes / 2;
    if (trimSamples < minTrimSamples)
      trimSamples = minTrimSamples;
    if (trimSamples > maxTrimSamples)
      trimSamples = maxTrimSamples;

    s_pcmBytesWritten -= (trimSamples * 2);
  }

#if AUDIO_NORMALIZE_ENABLED
  // --- Peak normalization ---
  // Scan the final (trimmed) clip for its true peak and apply one uniform
  // gain so the loudest sample lands near AUDIO_NORMALIZE_TARGET_PEAK. This
  // compensates for how close/far the user held the mic and hands Whisper a
  // consistently full-scale, un-clipped signal — which improves accuracy far
  // more reliably than a fixed capture gain. The gain is capped so a nearly
  // silent clip isn't blown up into amplified noise, and each scaled sample
  // is still clamped to int16 range as a final guard.
  if (s_pcmBytesWritten >= 2) {
    int16_t *norm = (int16_t *)(s_buffer + WAV_HEADER_SIZE_BYTES);
    size_t normSamples = s_pcmBytesWritten / 2;

    int16_t truePeak = 0;
    for (size_t i = 0; i < normSamples; i++) {
      int16_t a = norm[i] < 0 ? -norm[i] : norm[i];
      if (a > truePeak) truePeak = a;
    }

    if (truePeak > 0) {
      float scale = AUDIO_NORMALIZE_TARGET_PEAK / (float)truePeak;
      if (scale > AUDIO_NORMALIZE_MAX_GAIN) scale = AUDIO_NORMALIZE_MAX_GAIN;
      // Only bother if it makes a meaningful difference (avoids pointless work
      // and rounding churn when the clip is already near target).
      if (scale > 1.02f || scale < 0.98f) {
        for (size_t i = 0; i < normSamples; i++) {
          int32_t v = (int32_t)lrintf((float)norm[i] * scale);
          if (v > 32767) v = 32767;
          if (v < -32768) v = -32768;
          norm[i] = (int16_t)v;
        }
        Serial.printf("[Audio] Normalized: peak %d -> ~%d (x%.2f)\n", truePeak,
                      (int)(truePeak * scale), scale);
      }
    }
  }
#endif

  // Print comprehensive diagnostics for this recording
  double rms = 0.0;
  if (s_totalSamplesProcessed > 0) {
    rms = sqrt(s_sumSquares / s_totalSamplesProcessed);
  }
  float clipPercent = (s_totalSamplesProcessed > 0) ? ((float)s_numClippedSamples / s_totalSamplesProcessed) * 100.0f : 0.0f;
  float zeroPercent = (s_totalSamplesProcessed > 0) ? ((float)s_numZeroSamples / s_totalSamplesProcessed) * 100.0f : 0.0f;

  Serial.println(F("\n=== [Audio Diagnostics] ==="));
  Serial.printf("Duration:      %lu ms\n", getElapsedMs());
  Serial.printf("WAV Size:      %zu bytes\n", s_pcmBytesWritten + WAV_HEADER_SIZE_BYTES);
  Serial.printf("Peak Amp:      %d / 32768\n", s_peakAmplitude);
  Serial.printf("RMS Level:     %.2f\n", rms);
  Serial.printf("Clipped:       %lu samples (%.2f%%)\n", (unsigned long)s_numClippedSamples, clipPercent);
  Serial.printf("Absolute Zero: %lu samples (%.2f%%)\n", (unsigned long)s_numZeroSamples, zeroPercent);
  Serial.println(F("===========================\n"));

  // ---------------------------------------------------------------------
  // Speech gate: reject clips that are essentially silence/noise BEFORE
  // spending a network round-trip on them. Whisper reliably hallucinates
  // canned phrases (" Thank you.", "Thanks for watching.") when fed silence
  // or noise, so filtering here is what actually kills those bogus replies.
  // Thresholds are deliberately conservative so genuine short words still
  // pass; we combine three independent signals rather than trusting one:
  //   1. duration  — too short to be a word
  //   2. peak      — never rose above the noise floor
  //   3. RMS       — average energy too low for speech
  //   4. zero ratio— mostly dead samples (dropouts / no real signal)
  // RMS/peak are measured from the CAPTURED signal (pre-normalization), so
  // the normalizer can't mask a silent clip by amplifying its noise floor.
  // ---------------------------------------------------------------------
  const size_t MIN_SPEECH_BYTES =
      (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * 400) / 1000; // 400ms
  const int16_t MIN_SPEECH_PEAK = 800;  // below this the clip never left the noise floor
  const double MIN_RMS = 150.0;         // speech carries clearly more energy than this
  const float MAX_ZERO_PERCENT = 55.0f; // mostly-dead clip = dropout / no speech

  if (s_pcmBytesWritten < MIN_SPEECH_BYTES) {
    Serial.printf("[Audio Error] Rejecting clip: too short (%u bytes, need %u)\n",
                  (unsigned)s_pcmBytesWritten, (unsigned)MIN_SPEECH_BYTES);
    outSize = 0;
    return nullptr;
  }

  if (s_peakAmplitude < MIN_SPEECH_PEAK) {
    Serial.printf("[Audio Error] Rejecting clip: too quiet (peak %d, need %d)\n",
                  s_peakAmplitude, MIN_SPEECH_PEAK);
    outSize = 0;
    return nullptr;
  }

  if (rms < MIN_RMS) {
    Serial.printf("[Audio Error] Rejecting clip: RMS too low (%.2f, need %.2f)\n",
                  rms, MIN_RMS);
    outSize = 0;
    return nullptr;
  }

  if (zeroPercent > MAX_ZERO_PERCENT) {
    Serial.printf("[Audio Error] Rejecting clip: %.1f%% silent samples (max %.1f%%) "
                  "- no continuous speech\n", zeroPercent, MAX_ZERO_PERCENT);
    outSize = 0;
    return nullptr;
  }

  const uint32_t dataChunkSize = (uint32_t)s_pcmBytesWritten;
  const uint32_t riffChunkSize = 36 + dataChunkSize; // 36 = header size - 8
  const uint16_t audioFormatPCM = 1;
  const uint16_t numChannels = AUDIO_NUM_CHANNELS;
  const uint32_t sampleRate = AUDIO_SAMPLE_RATE_HZ;
  const uint16_t bitsPerSample = AUDIO_BITS_PER_SAMPLE;
  const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  const uint16_t blockAlign = numChannels * (bitsPerSample / 8);

  uint8_t *h = s_buffer;
  memcpy(h + 0, "RIFF", 4);
  writeLE32(h, 4, riffChunkSize);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  writeLE32(h, 16, 16); // fmt chunk size for PCM
  writeLE16(h, 20, audioFormatPCM);
  writeLE16(h, 22, numChannels);
  writeLE32(h, 24, sampleRate);
  writeLE32(h, 28, byteRate);
  writeLE16(h, 32, blockAlign);
  writeLE16(h, 34, bitsPerSample);
  memcpy(h + 36, "data", 4);
  writeLE32(h, 40, dataChunkSize);

  outSize = WAV_HEADER_SIZE_BYTES + s_pcmBytesWritten;

  // NOTE: we no longer realloc()-shrink the buffer here. The buffer is
  // now a fixed, permanently-allocated block (see begin()); repeatedly
  // shrinking and re-growing it via realloc() on every recording was
  // another source of heap fragmentation. ai.cpp only reads `outSize`
  // bytes starting at this pointer, so the buffer being larger than the
  // WAV data it currently holds is harmless.
  return s_buffer;
}

void releaseBuffer() {
  // Free the recording buffer to reclaim heap for TLS / JSON parsing.
  // On no-PSRAM boards this is critical: the ~64 KB buffer and the
  // ~45 KB TLS session cannot coexist in the ~320 KB internal SRAM.
  // startRecording() will re-allocate it when the user presses the
  // button again.
  if (s_buffer != nullptr) {
    free(s_buffer);
    s_buffer = nullptr;
    Serial.println(F("[Audio] Recording buffer FREED"));
    printHeapDiag("releaseBuffer()");
  }
}

} // namespace FayasAudio