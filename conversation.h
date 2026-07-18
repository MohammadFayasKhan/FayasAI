/**
 * @file    conversation.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   Lightweight multi-turn conversation memory.
 *
 *          Keeps a small ring buffer of the most recent user/assistant
 *          exchanges so the chat model can answer follow-up questions with
 *          context ("what about the other one?", "say that again"). The
 *          history is intentionally bounded (see CONV_* in config.h) so it
 *          never threatens the ESP32's limited RAM or the model's context
 *          budget: oldest turns are dropped first, and each message is
 *          length-capped when stored.
 *
 *          Unlike xiaozhi (which keeps context on its own server across a
 *          persistent WebSocket), Fayas AI is serverless, so the device
 *          itself owns the rolling window and replays it on every request.
 */

#ifndef CONVERSATION_H
#define CONVERSATION_H

#include <Arduino.h>

namespace FayasConversation {

/// One stored exchange: what the user said and how the assistant replied.
struct Turn {
  String user;
  String assistant;
};

/// Reset internal state. Call once from setup().
void begin();

/**
 * @brief Record a completed exchange. Both strings are truncated to their
 *        configured character caps before storage, and the oldest turn is
 *        evicted if the ring buffer is full.
 */
void addTurn(const String &userText, const String &assistantText);

/**
 * @brief Drop all stored history (e.g. session timed out or user asked to
 *        start over).
 */
void clear();

/// Number of turns currently held.
size_t count();

/**
 * @brief Fetch a stored turn by index, 0 = oldest still-retained turn.
 * @return true if @p index is valid and @p out was populated.
 */
bool getTurn(size_t index, Turn &out);

/**
 * @brief Decide whether the existing history should be discarded because the
 *        session has been idle longer than CONV_SESSION_TIMEOUT_MS. Call this
 *        just before starting a new interaction; it clears history internally
 *        when stale and returns true if it did so.
 */
bool expireIfStale();

/// Timestamp (millis) of the most recent recorded turn, for staleness checks.
unsigned long lastActivityMs();

} // namespace FayasConversation

#endif // CONVERSATION_H
