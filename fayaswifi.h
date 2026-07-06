/**
 * @file    fayaswifi.h
 * @project Fayas AI
 * @author  Fayas
 * @version 1.0.0
 * @brief   Wi-Fi connection management: initial connect with timeout,
 *          background health checks, and automatic reconnection.
 *
 * NOTE: This file is intentionally named "fayaswifi.h" (not "wifi.h")
 *       to avoid a case-insensitive filename collision with the ESP32
 *       SDK's <WiFi.h> on macOS, which treats "wifi.h" and "WiFi.h"
 *       as the same file, causing the SDK's WiFi class to never load.
 */

#ifndef FAYAS_WIFI_H
#define FAYAS_WIFI_H

#include <Arduino.h>

namespace FayasWiFi {

/**
 * @brief Attempt to join the network configured in config.h.
 * @return true if connected before WIFI_CONNECT_TIMEOUT_MS elapses,
 *         false on timeout (caller should show the error screen).
 * @note Blocks the caller only for the boot-time connect attempt (there is
 *       no meaningful animation to run before Wi-Fi exists, since the
 *       device cannot proceed without it). All post-boot checks are
 *       non-blocking via isHealthy()/reconnectIfNeeded().
 */
bool connect();

/// True if WiFi.status() == WL_CONNECTED right now.
bool isConnected();

/**
 * @brief Non-blocking watchdog: call every loop() iteration. Internally
 *        rate-limited to WIFI_CHECK_INTERVAL_MS so it doesn't spam
 *        WiFi.status() checks or reconnect attempts.
 * @return true if a reconnect was just successfully completed (so the
 *         caller can, e.g., leave the error screen), false otherwise.
 */
bool reconnectIfNeeded();

/// Current signal strength in dBm, or 0 if not connected.
long getRSSI();

/// Local IP address as a string, or "0.0.0.0" if not connected.
String getLocalIP();

} // namespace FayasWiFi

#endif // FAYAS_WIFI_H
