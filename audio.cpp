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
  s_bufferCapacity = WAV_HEADER_SIZE_BYTES + AUDIO_MAX_BUFFER_BYTES;

  printHeapDiag("audio.begin() entry");
  Serial.printf("[Audio] PSRAM detected: %s\n", s_hasPSRAM ? "YES" : "NO");
  Serial.printf("[Audio] Recording buffer capacity: %u bytes (allocated on demand)\n",
                s_bufferCapacity);

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
    s_buffer = allocateAudioBuffer(s_bufferCapacity);
    if (s_buffer == nullptr) {
      Serial.println(F("[Audio] ERROR: could not allocate recording buffer!"));
      printHeapDiag("startRecording() alloc FAILED");
      s_recording = false;
      return;
    }
    Serial.printf("[Audio] Allocated recording buffer: %u bytes\n",
                  s_bufferCapacity);
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

  size_t remainingCapacity = AUDIO_MAX_BUFFER_BYTES - s_pcmBytesWritten;
  if (remainingCapacity == 0) {
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

  if (result == ESP_OK && bytesRead > 0) {
    size_t samplesRead = bytesRead / 4;
    int32_t *samples32 = (int32_t *)s_readChunk;

    int16_t *dest16 =
        (int16_t *)(s_buffer + WAV_HEADER_SIZE_BYTES + s_pcmBytesWritten);
    size_t samplesWritten = 0;

    int16_t peakMin = 32767;
    int16_t peakMax = -32768;

#if AUDIO_SAMPLE_RATE_HZ == 8000
    const float R = 0.94f; // ~80Hz cutoff at 8kHz sample rate
#else
    const float R = 0.97f; // ~80Hz cutoff at 12kHz/16kHz sample rate
#endif
    const float LIMITER_THRESHOLD = 24000.0f;
    const float LIMITER_KNEE = 8000.0f;

    // Loop through the mono read samples
    for (size_t i = 0; i < samplesRead; i++) {
      // The ESP32 I2S driver receives 24-bit data left-justified in a 32-bit register.
      // So the 24-bits are in bits [31:8]. Shifting right by 16 gives us the most significant
      // 16 bits of the audio data in a 16-bit signed format.
      float raw = (float)(samples32[i] >> 16);

      // DC blocking high-pass filter formula
      float filtered = raw - s_lastRaw + R * s_lastFiltered;
      s_lastRaw = raw;
      s_lastFiltered = filtered;

      // Apply digital gain multiplier to boost volume
      filtered *= AUDIO_GAIN_MULTIPLIER;

      // Soft-knee limiter: compress values
      float absVal = fabsf(filtered);
      if (absVal > LIMITER_THRESHOLD) {
        float over = absVal - LIMITER_THRESHOLD;
        float compressed =
            LIMITER_THRESHOLD + (LIMITER_KNEE * over) / (over + LIMITER_KNEE);
        filtered = (filtered > 0.0f) ? compressed : -compressed;
      }

      int32_t val = (int32_t)filtered;
      bool clipped = false;
      if (val > 32767) {
        val = 32767;
        clipped = true;
      }
      if (val < -32768) {
        val = -32768;
        clipped = true;
      }

      int16_t converted = (int16_t)val;
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
    }

    // Print peak values every ~10 chunks
    static int s_logCounter = 0;
    if (s_logCounter++ % 5 == 0 && samplesWritten > 0) {
      Serial.printf("[Audio Pump] Captured chunk peak: Min=%d, Max=%d\n",
                    peakMin, peakMax);
    }

    s_pcmBytesWritten += (samplesWritten * 2); // 2 bytes written per sample
  }

  if (s_pcmBytesWritten >= AUDIO_MAX_BUFFER_BYTES) {
    stopRecording();
    return true;
  }
  return false;
}

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

  // Reject clips that can't possibly be real speech
  const size_t MIN_SPEECH_BYTES =
      (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8) * 400) / 1000; // 400ms
  const int16_t MIN_SPEECH_PEAK = 500; // below this, the clip is essentially silence/noise-floor
  const double MIN_RMS = 50.0; // Needs to have some energy

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