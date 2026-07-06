/**
 * @file    fayaswifi.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of Wi-Fi connect/reconnect logic.
 */

#include "fayaswifi.h"
#include <WiFi.h>
#include "config.h"

namespace FayasWiFi {

// Tracks the last time we checked connection health, so reconnectIfNeeded()
// can be called every loop() iteration without hammering WiFi.status().
static unsigned long s_lastCheckMs = 0;
static bool s_reconnecting = false;

bool connect() {
    // Purpose: Join the configured Wi-Fi network at boot.
    // Inputs: none (uses WIFI_SSID / WIFI_PASSWORD from config.h).
    // Outputs: true on success, false if WIFI_CONNECT_TIMEOUT_MS elapses
    //          without reaching WL_CONNECTED.
    // Logic: WiFi.begin() is asynchronous; we poll WiFi.status() until it
    //        reports connected or we hit the timeout.
    // Possible errors: wrong SSID/password (will time out), router out of
    //        range (will time out), or radio hardware failure (status
    //        will remain WL_NO_SHIELD/WL_DISCONNECTED and we time out the
    //        same way - the caller shows a generic error screen either way).
    WiFi.mode(WIFI_STA);
    // Set custom DNS (Google 8.8.8.8 and Cloudflare 1.1.1.1) to prevent mobile hotspot DNS drops
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((millis() - startMs) >= WIFI_CONNECT_TIMEOUT_MS) {
            return false;
        }
        delay(100); // Acceptable here: boot-time only, before any animation loop starts.
    }

    s_lastCheckMs = millis();
    return true;
}

bool isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool reconnectIfNeeded() {
    // Purpose: Non-blocking watchdog for Wi-Fi health, called every loop().
    // Inputs: none. Outputs: true exactly on the iteration a reconnect
    //         attempt just succeeded; false otherwise (including the
    //         common case where nothing needed checking yet).
    // Logic: Rate-limited by WIFI_CHECK_INTERVAL_MS. If disconnected,
    //        kicks off WiFi.reconnect() once and lets it proceed in the
    //        background rather than blocking the animation loop.
    // Possible errors: If the router is gone permanently, this will retry
    //        forever at the check interval; the caller (main.ino) is
    //        responsible for showing the error screen while
    //        isConnected() == false so the user isn't left guessing.
    unsigned long now = millis();
    if ((now - s_lastCheckMs) < WIFI_CHECK_INTERVAL_MS) {
        return false;
    }
    s_lastCheckMs = now;

    if (WiFi.status() == WL_CONNECTED) {
        if (s_reconnecting) {
            s_reconnecting = false;
            return true; // Just recovered.
        }
        return false;
    }

    // Not connected: kick off (or continue) a reconnect attempt.
    s_reconnecting = true;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return false;
}

long getRSSI() {
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    return WiFi.RSSI();
}

String getLocalIP() {
    if (WiFi.status() != WL_CONNECTED) {
        return String("0.0.0.0");
    }
    return WiFi.localIP().toString();
}

} // namespace FayasWiFi
