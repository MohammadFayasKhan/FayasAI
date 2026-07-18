/**
 * @file    conversation.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of the bounded multi-turn conversation memory.
 *
 * Storage model: a fixed-size circular array of CONV_MAX_TURNS Turn slots.
 * s_count tracks how many are populated (<= CONV_MAX_TURNS); once full,
 * s_head advances so the oldest turn is overwritten. This keeps memory flat
 * and predictable — no dynamic growth, no fragmentation from repeated
 * allocation, only the Strings themselves resize within their char caps.
 */

#include "conversation.h"
#include "config.h"

namespace FayasConversation {

static Turn s_turns[CONV_MAX_TURNS];
static size_t s_count = 0;      // number of populated slots (0..CONV_MAX_TURNS)
static size_t s_head = 0;       // index of the OLDEST retained turn
static unsigned long s_lastActivityMs = 0;

/// Copy at most @p maxChars characters of @p src into a String, cutting on a
/// UTF-8 boundary is not attempted (Whisper/LLM tolerate a clipped tail), but
/// we avoid slicing mid-multibyte where cheap by trimming trailing high bytes.
static String capString(const String &src, size_t maxChars) {
  if (src.length() <= maxChars) {
    return src;
  }
  String out = src.substring(0, maxChars);
  // Avoid leaving a dangling partial multi-byte UTF-8 sequence at the very
  // end (bytes with the high bit set). Trim back to the last clean boundary.
  int i = out.length() - 1;
  while (i >= 0 && ((uint8_t)out[i] & 0x80)) {
    out.remove(i);
    i--;
  }
  out.trim();
  return out;
}

void begin() {
  clear();
}

void clear() {
  for (size_t i = 0; i < CONV_MAX_TURNS; i++) {
    s_turns[i].user = String();
    s_turns[i].assistant = String();
  }
  s_count = 0;
  s_head = 0;
  // Note: we deliberately leave s_lastActivityMs untouched so lastActivityMs()
  // still reflects the true last interaction time for external callers.
}

void addTurn(const String &userText, const String &assistantText) {
#if !CONV_MEMORY_ENABLED
  (void)userText;
  (void)assistantText;
  return;
#else
  size_t slot;
  if (s_count < CONV_MAX_TURNS) {
    // Append into the next free slot (physical index = head + count).
    slot = (s_head + s_count) % CONV_MAX_TURNS;
    s_count++;
  } else {
    // Buffer full: overwrite the oldest slot and advance the head.
    slot = s_head;
    s_head = (s_head + 1) % CONV_MAX_TURNS;
  }

  s_turns[slot].user = capString(userText, CONV_MAX_USER_CHARS);
  s_turns[slot].assistant = capString(assistantText, CONV_MAX_ASSISTANT_CHARS);
  s_lastActivityMs = millis();
#endif
}

size_t count() {
  return s_count;
}

bool getTurn(size_t index, Turn &out) {
  if (index >= s_count) {
    return false;
  }
  size_t slot = (s_head + index) % CONV_MAX_TURNS;
  out = s_turns[slot];
  return true;
}

bool expireIfStale() {
#if CONV_SESSION_TIMEOUT_MS == 0UL
  return false; // Never auto-expire.
#else
  if (s_count == 0) {
    return false;
  }
  unsigned long now = millis();
  // millis() wraps every ~49 days; the unsigned subtraction handles wrap
  // correctly for our purposes.
  if ((now - s_lastActivityMs) > CONV_SESSION_TIMEOUT_MS) {
    clear();
    return true;
  }
  return false;
#endif
}

unsigned long lastActivityMs() {
  return s_lastActivityMs;
}

} // namespace FayasConversation
