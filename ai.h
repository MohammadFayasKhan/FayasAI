/**
 * @file    ai.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   HTTPS client for the AI Assistant API (STT + Chat).
 */

#ifndef AI_H
#define AI_H

#include <Arduino.h>

namespace FayasAI {

/**
 * @brief Optional callback invoked periodically while sendAudio() is
 *        streaming the request body or waiting for a response.
 */
typedef void (*ProgressCallback)();

/// Machine-readable outcome of a sendAudio() call, used to pick the right
/// error message on the Error screen.
enum class AIResult : uint8_t {
    OK,
    ERR_WIFI_DISCONNECTED,
    ERR_CONNECT_FAILED,
    ERR_HTTP_STATUS,     // Non-2xx HTTP response
    ERR_TIMEOUT,
    ERR_MALFORMED_JSON,
    ERR_EMPTY_RESPONSE
};

/**
 * @brief Send recorded WAV audio to the STT API and retrieve the text response.
 */
AIResult sendAudio(const uint8_t *wavData, size_t wavSize,
                     String &outResponse, unsigned long &outLatencyMs,
                     ProgressCallback onProgress = nullptr);

/// Human-readable message for a given result code, for the Error screen.
const char *resultToMessage(AIResult result);

} // namespace FayasAI

#endif // AI_H
