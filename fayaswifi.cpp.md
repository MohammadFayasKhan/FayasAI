# fayaswifi.cpp

The implementation of the WiFi connection manager for the ESP32. It handles network connections, background watchdogs, and signal strength (RSSI) queries.

---

## 🗺️ Reconnection Watchdog Logic

```mermaid
graph TD
    TriggerCheck[Watchdog Timer Checked] --> ConnectCheck{WiFi.status() == WL_CONNECTED?}
    
    ConnectCheck -->|Yes| ExitCheck[Reset Reconnect Timer & Return]
    ConnectCheck -->|No| IntervalCheck{Elapsed > 10 seconds?}
    
    IntervalCheck -->|No| ExitCheck
    IntervalCheck -->|Yes| Reconnect[Call WiFi.reconnect()]
    
    Reconnect --> LogState[Print Reconnect Attempt to Serial]
    LogState --> ResetTimer[Reset Reconnect Timer]
    ResetTimer --> ExitCheck
```

---

## ⚙️ Core Operations

### 1. Connection Initialization
- `begin()` puts the ESP32 WiFi radio into Station Mode (`WIFI_STA`) to disable access-point broadcasting and save power.
- Initiates the connection using credentials in `config.h`.

### 2. Auto-Reconnection Watchdog
- `reconnectIfNeeded()` checks the network status using a timer to avoid blocking the main loop:
  - If the link drops, it prints a message and attempts to reconnect.
  - Limits reconnect attempts to avoid stalling or blocking application threads.

### 3. Signal Strength Queries
- `getRSSI()` returns the signal strength of the current network connection.
- If the connection drops or is unavailable, it returns `0`.
- Used to calculate and draw signal bars on the OLED display in real-time.
