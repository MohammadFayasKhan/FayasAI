/**
 * @file    ai.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of the Groq API client (Speech-to-Text + Chat Completion).
 */

#include "ai.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "fayaswifi.h"

namespace FayasAI {

// Safely invokes the optional screen-rendering callback so the "Thinking"
// animation keeps moving during long blocking operations.
static inline void tick(ProgressCallback onProgress) {
    if (onProgress != nullptr) {
        onProgress();
    }
}

// Forward declarations
static bool readHttpStatusAndHeaders(WiFiClientSecure &client, int &outStatusCode, size_t &outContentLength,
                                      unsigned long timeoutMs, ProgressCallback onProgress);

AIResult sendAudio(const uint8_t *wavData, size_t wavSize,
                      String &outResponse, unsigned long &outLatencyMs,
                      ProgressCallback onProgress) {
    unsigned long startMs = millis();
    outLatencyMs = 0;

    if (WiFi.status() != WL_CONNECTED) {
        return AIResult::ERR_WIFI_DISCONNECTED;
    }
    if (wavData == nullptr || wavSize == 0) {
        return AIResult::ERR_EMPTY_RESPONSE;
    }

    String transcribedTextStr;

    // ========================================================================
    // STEP 1: Groq Whisper Speech-to-Text Transcription (Isolated Scope)
    // ========================================================================
    {
        WiFiClientSecure client;
        client.setInsecure(); // Skip certificate verification for speed and simplicity
        client.setTimeout(AI_HTTP_TIMEOUT_MS / 1000);

        Serial.println(F("[Groq STT] Connecting to api.groq.com..."));
        if (!client.connect(AI_API_HOST, AI_API_PORT)) {
            char errBuf[128] = {0};
            client.lastError(errBuf, sizeof(errBuf));
            Serial.printf("[Groq STT] Connection failed! Error: %s\n", errBuf);
            return AIResult::ERR_CONNECT_FAILED;
        }
        tick(onProgress);

        String boundary = "---------------------------ESP32Boundary";
        String partModel = "--" + boundary + "\r\n" +
                           "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
                           GROQ_WHISPER_MODEL + "\r\n";

        String partLanguage = "";
#ifdef GROQ_WHISPER_LANGUAGE
        if (strlen(GROQ_WHISPER_LANGUAGE) > 0) {
            partLanguage = "--" + boundary + "\r\n" +
                           "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
                           GROQ_WHISPER_LANGUAGE + "\r\n";
        }
#endif

        String partPrompt = "";
#ifdef GROQ_WHISPER_PROMPT
        if (strlen(GROQ_WHISPER_PROMPT) > 0) {
            partPrompt = "--" + boundary + "\r\n" +
                         "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n" +
                         GROQ_WHISPER_PROMPT + "\r\n";
        }
#endif

        // temperature=0 makes Whisper decode greedily (most-likely tokens only)
        String partTemperature = "--" + boundary + "\r\n" +
                                 "Content-Disposition: form-data; name=\"temperature\"\r\n\r\n" +
                                 "0\r\n";

        String partFileHeader = "--" + boundary + "\r\n" +
                                "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n" +
                                "Content-Type: audio/wav\r\n\r\n";
        String partFileFooter = "\r\n--" + boundary + "--\r\n";

        size_t whisperContentLength = partModel.length() + partLanguage.length() + partPrompt.length() + partTemperature.length() + partFileHeader.length() + wavSize + partFileFooter.length();

        // Send HTTP Headers for Whisper
        client.println(F("POST /openai/v1/audio/transcriptions HTTP/1.1"));
        client.printf("Host: %s\r\n", AI_API_HOST);
        client.printf("Authorization: Bearer %s\r\n", FayasWiFi::getApiKey().c_str());
        client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
        client.printf("Content-Length: %d\r\n", whisperContentLength);
        client.println(F("Connection: close"));
        client.println();

        // Stream Whisper Body
        client.print(partModel);
        tick(onProgress);
        if (partLanguage.length() > 0) {
            client.print(partLanguage);
            tick(onProgress);
        }
        if (partPrompt.length() > 0) {
            client.print(partPrompt);
            tick(onProgress);
        }
        client.print(partTemperature);
        tick(onProgress);
        client.print(partFileHeader);
        tick(onProgress);

        // Stream WAV binary data in 1KB chunks to keep memory usage low
        size_t offset = 0;
        const size_t CHUNK_SIZE = 1024;
        while (offset < wavSize) {
            size_t writeLen = min(CHUNK_SIZE, wavSize - offset);
            client.write(wavData + offset, writeLen);
            offset += writeLen;
            tick(onProgress);
        }
        client.print(partFileFooter);
        tick(onProgress);

        // Read Response Status
        int whisperStatusCode = 0;
        size_t whisperResponseLength = 0;
        if (!readHttpStatusAndHeaders(client, whisperStatusCode, whisperResponseLength, AI_HTTP_TIMEOUT_MS, onProgress)) {
            Serial.println(F("[Groq STT] Header read timeout"));
            client.stop();
            outLatencyMs = millis() - startMs;
            return AIResult::ERR_TIMEOUT;
        }

        if (whisperStatusCode < 200 || whisperStatusCode >= 300) {
            Serial.printf("[Groq STT] Request failed. HTTP Status Code: %d\n", whisperStatusCode);
            String errBody;
            while (client.connected() || client.available()) {
                if (client.available()) errBody += (char)client.read();
            }
            Serial.print(F("[Groq STT] Error Response: "));
            Serial.println(errBody);
            client.stop();
            outLatencyMs = millis() - startMs;
            return AIResult::ERR_HTTP_STATUS;
        }

        // Read Transcription Response Body
        String whisperBody;
        if (whisperResponseLength > 0) {
            whisperBody.reserve(whisperResponseLength + 1);
            unsigned long bodyStart = millis();
            while (whisperBody.length() < whisperResponseLength) {
                if ((millis() - bodyStart) > AI_HTTP_TIMEOUT_MS) {
                    Serial.println(F("[Groq STT] Body read timeout"));
                    client.stop();
                    outLatencyMs = millis() - startMs;
                    return AIResult::ERR_TIMEOUT;
                }
                if (client.available()) {
                    whisperBody += (char)client.read();
                } else {
                    tick(onProgress);
                }
            }
        } else {
            // Fallback: read until connection is closed
            whisperBody.reserve(512);
            while (client.connected() || client.available()) {
                if (client.available()) {
                    whisperBody += (char)client.read();
                } else {
                    tick(onProgress);
                }
            }
        }
        client.stop(); // Cleanly close connection to trigger SSL cleanup

        // Parse transcribed text
        JsonDocument whisperDoc;
        DeserializationError err = deserializeJson(whisperDoc, whisperBody);
        if (err) {
            Serial.printf("[Groq STT] JSON Deserialization Error: %s\n", err.c_str());
            Serial.print(F("[Groq STT] Response: "));
            Serial.println(whisperBody);
            return AIResult::ERR_MALFORMED_JSON;
        }

        const char* transcribedText = whisperDoc["text"];
        if (transcribedText == nullptr) {
            Serial.println(F("[Groq STT] Error: 'text' field missing in JSON response"));
            return AIResult::ERR_MALFORMED_JSON;
        }

        Serial.printf("[Groq STT] Transcribed text: \"%s\"\n", transcribedText);

        // Check if transcription is empty
        if (strlen(transcribedText) == 0) {
            Serial.println(F("[Groq STT] Silence or no audio recognized."));
            return AIResult::ERR_EMPTY_RESPONSE;
        }

        // Copy content before whisperDoc and client go out of scope
        transcribedTextStr = String(transcribedText);
    } // Whisper client is destroyed here, reclaiming ~40KB SSL heap buffers

    // Yield execution and let the ESP32 heap defragment
    delay(150);

    // ========================================================================
    // STEP 2: Groq Chat Completion (Llama Model - Fresh SSL connection)
    // ========================================================================
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(AI_HTTP_TIMEOUT_MS / 1000);

    Serial.println(F("[Groq Chat] Connecting to api.groq.com..."));
    if (!client.connect(AI_API_HOST, AI_API_PORT)) {
        char errBuf[128] = {0};
        client.lastError(errBuf, sizeof(errBuf));
        Serial.printf("[Groq Chat] Connection failed! Error: %s\n", errBuf);
        return AIResult::ERR_CONNECT_FAILED;
    }
    tick(onProgress);

    // Build Chat JSON Payload
    JsonDocument chatPayload;
    chatPayload["model"] = GROQ_LLM_MODEL;
    chatPayload["max_tokens"] = 80; // Limit output size to prevent long generations and save time
    
    JsonArray messages = chatPayload["messages"].to<JsonArray>();
    
    JsonObject sysMsg = messages.add<JsonObject>();
    sysMsg["role"] = "system";
    sysMsg["content"] = AI_SYSTEM_PROMPT;
    
    JsonObject userMsg = messages.add<JsonObject>();
    userMsg["role"] = "user";
    userMsg["content"] = transcribedTextStr;

    String chatJson;
    serializeJson(chatPayload, chatJson);

    // Send HTTP Headers for Chat
    client.println(F("POST /openai/v1/chat/completions HTTP/1.1"));
    client.printf("Host: %s\r\n", AI_API_HOST);
    client.printf("Authorization: Bearer %s\r\n", FayasWiFi::getApiKey().c_str());
    client.println(F("Content-Type: application/json"));
    client.printf("Content-Length: %d\r\n", chatJson.length());
    client.println(F("Connection: close")); // Close connection after response
    client.println();

    // Send Chat Body
    client.print(chatJson);
    tick(onProgress);

    // Read Response Status
    int chatStatusCode = 0;
    size_t chatContentLength = 0;
    if (!readHttpStatusAndHeaders(client, chatStatusCode, chatContentLength, AI_HTTP_TIMEOUT_MS, onProgress)) {
        Serial.println(F("[Groq Chat] Header read timeout"));
        client.stop();
        outLatencyMs = millis() - startMs;
        return AIResult::ERR_TIMEOUT;
    }

    if (chatStatusCode < 200 || chatStatusCode >= 300) {
        Serial.printf("[Groq Chat] Request failed. HTTP Status Code: %d\n", chatStatusCode);
        String errBody;
        while (client.connected() || client.available()) {
            if (client.available()) errBody += (char)client.read();
        }
        Serial.print(F("[Groq Chat] Error Response: "));
        Serial.println(errBody);
        client.stop();
        outLatencyMs = millis() - startMs;
        return AIResult::ERR_HTTP_STATUS;
    }

    // Read Chat Response Body
    String chatBody;
    chatBody.reserve(1024);
    while (client.connected() || client.available()) {
        if (client.available()) {
            chatBody += (char)client.read();
        } else {
            tick(onProgress);
        }
    }
    client.stop();
    outLatencyMs = millis() - startMs;

    // Parse Chat Reply
    JsonDocument chatDoc;
    DeserializationError err = deserializeJson(chatDoc, chatBody);
    if (err) {
        Serial.printf("[Groq Chat] JSON Deserialization Error: %s\n", err.c_str());
        Serial.print(F("[Groq Chat] Response: "));
        Serial.println(chatBody);
        return AIResult::ERR_MALFORMED_JSON;
    }

    const char* replyText = chatDoc["choices"][0]["message"]["content"];
    if (replyText == nullptr) {
        Serial.println(F("[Groq Chat] Error: 'choices[0].message.content' field missing in JSON response"));
        return AIResult::ERR_MALFORMED_JSON;
    }

    outResponse = String(replyText);
    return AIResult::OK;
}

static bool readHttpStatusAndHeaders(WiFiClientSecure &client, int &outStatusCode, size_t &outContentLength,
                                      unsigned long timeoutMs, ProgressCallback onProgress) {
    outContentLength = 0;
    unsigned long start = millis();
    String statusLine;
    while (statusLine.length() == 0) {
        if ((millis() - start) > timeoutMs) return false;
        if (client.available()) {
            statusLine = client.readStringUntil('\n');
        } else {
            tick(onProgress);
        }
    }
    int firstSpace = statusLine.indexOf(' ');
    int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
    if (firstSpace < 0 || secondSpace < 0) return false;
    outStatusCode = statusLine.substring(firstSpace + 1, secondSpace).toInt();

    while (true) {
        if ((millis() - start) > timeoutMs) return false;
        if (client.available()) {
            String line = client.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) {
                break;
            }
            if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
                int colonIdx = line.indexOf(':');
                if (colonIdx > 0) {
                    outContentLength = line.substring(colonIdx + 1).toInt();
                }
            }
        } else {
            tick(onProgress);
        }
    }
    return true;
}

const char *resultToMessage(AIResult result) {
    switch (result) {
        case AIResult::OK:                    return "OK";
        case AIResult::ERR_WIFI_DISCONNECTED:  return "WiFi Lost\nReconnect...";
        case AIResult::ERR_CONNECT_FAILED:     return "Connect Failed\nCheck Network";
        case AIResult::ERR_HTTP_STATUS:        return "API Error\nCheck Key/Quota";
        case AIResult::ERR_TIMEOUT:            return "Timeout\nTry Again";
        case AIResult::ERR_MALFORMED_JSON:     return "Bad Response\nTry Again";
        case AIResult::ERR_EMPTY_RESPONSE:     return "No Response\nTry Again";
        default:                                   return "Unknown Error";
    }
}

} // namespace FayasAI
