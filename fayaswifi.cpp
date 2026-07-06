/**
 * @file    fayaswifi.cpp
 * @project Fayas AI
 * @author  Fayas
 * @brief   Implementation of Wi-Fi connect/reconnect logic and fallback config portal.
 */

#include "fayaswifi.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "config.h"

namespace FayasWiFi {

// Tracks the last time we checked connection health, so reconnectIfNeeded()
// can be called every loop() iteration without hammering WiFi.status().
static unsigned long s_lastCheckMs = 0;
static bool s_reconnecting = false;

// Dynamic credentials cached after loading from Preferences
static String s_ssid = "";
static String s_password = "";
static String s_apiKey = "";
static String s_portalErrorMsg = "";

// Web Server instance for the config portal
static WebServer s_server(80);

// Premium iOS-style Glassmorphic configuration page served by the AP
static const char CONFIG_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Fayas AI Configuration Portal</title>
    <style>
        :root {
            --bg-color: #0F0C20;
            --card-bg: rgba(255, 255, 255, 0.05);
            --border-color: rgba(255, 255, 255, 0.1);
            --primary-glow: #00D2FC;
            --secondary-glow: #8A2BE2;
            --text-color: #FFFFFF;
            --error-color: #FF4B4B;
        }
        body {
            margin: 0;
            padding: 0;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background: linear-gradient(135deg, #0F0C20 0%, #15102A 100%);
            color: var(--text-color);
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            overflow-x: hidden;
        }
        .blur-circle1 {
            position: absolute;
            width: 300px;
            height: 300px;
            border-radius: 50%;
            background: radial-gradient(circle, var(--primary-glow) 0%, rgba(0, 210, 252, 0) 70%);
            top: 10%;
            left: 10%;
            z-index: 1;
            filter: blur(80px);
            opacity: 0.3;
        }
        .blur-circle2 {
            position: absolute;
            width: 300px;
            height: 300px;
            border-radius: 50%;
            background: radial-gradient(circle, var(--secondary-glow) 0%, rgba(138, 43, 226, 0) 70%);
            bottom: 10%;
            right: 10%;
            z-index: 1;
            filter: blur(80px);
            opacity: 0.3;
        }
        .container {
            width: 90%;
            max-width: 450px;
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            border-radius: 20px;
            padding: 40px 30px;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.3);
            z-index: 2;
            box-sizing: border-box;
            animation: fadeIn 0.8s ease-in-out;
        }
        @keyframes fadeIn {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }
        h2 {
            margin-top: 0;
            margin-bottom: 10px;
            font-weight: 700;
            text-align: center;
            background: linear-gradient(90deg, var(--primary-glow), var(--secondary-glow));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            font-size: 28px;
            letter-spacing: 0.5px;
        }
        .subtitle {
            text-align: center;
            font-size: 14px;
            color: rgba(255, 255, 255, 0.6);
            margin-bottom: 30px;
        }
        .status-box {
            background: rgba(255, 75, 75, 0.1);
            border: 1px solid var(--error-color);
            border-radius: 12px;
            padding: 15px;
            font-size: 14px;
            color: #FF6B6B;
            margin-bottom: 25px;
            line-height: 1.4;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            font-size: 13px;
            font-weight: 600;
            margin-bottom: 8px;
            color: rgba(255, 255, 255, 0.8);
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        input[type="text"], input[type="password"] {
            width: 100%;
            padding: 14px 16px;
            background: rgba(0, 0, 0, 0.3);
            border: 1px solid var(--border-color);
            border-radius: 10px;
            color: #fff;
            font-size: 15px;
            transition: all 0.3s ease;
            box-sizing: border-box;
            outline: none;
        }
        input[type="text"]:focus, input[type="password"]:focus {
            border-color: var(--primary-glow);
            box-shadow: 0 0 10px rgba(0, 210, 252, 0.2);
            background: rgba(0, 0, 0, 0.5);
        }
        button {
            width: 100%;
            padding: 15px;
            background: linear-gradient(90deg, #00C6FF 0%, #0072FF 100%);
            border: none;
            border-radius: 10px;
            color: #fff;
            font-size: 16px;
            font-weight: 700;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(0, 114, 255, 0.3);
            margin-top: 10px;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(0, 114, 255, 0.5);
            background: linear-gradient(90deg, #00D2FC 0%, #0082FF 100%);
        }
        button:active {
            transform: translateY(1px);
        }
        .footer {
            margin-top: 30px;
            text-align: center;
            font-size: 11px;
            color: rgba(255, 255, 255, 0.4);
            letter-spacing: 0.5px;
        }
    </style>
</head>
<body>
    <div class="blur-circle1"></div>
    <div class="blur-circle2"></div>
    <div class="container">
        <h2>Fayas AI</h2>
        <div class="subtitle">Configuration Portal</div>
        
        %STATUS_PLACEHOLDER%

        <form action="/save" method="POST">
            <div class="form-group">
                <label for="ssid">Wi-Fi Network Name (SSID)</label>
                <input type="text" id="ssid" name="ssid" placeholder="Enter your Wi-Fi name" value="%SSID_VAL%" required>
            </div>
            
            <div class="form-group">
                <label for="password">Wi-Fi Password</label>
                <input type="password" id="password" name="password" placeholder="Enter network password" value="%PASSWORD_VAL%">
            </div>
            
            <div class="form-group">
                <label for="apikey">Groq API Key</label>
                <input type="text" id="apikey" name="apikey" placeholder="gsk_..." value="%APIKEY_VAL%" required>
            </div>
            
            <button type="submit">SAVE CONFIGURATION</button>
        </form>
        
        <div class="footer">POWERED BY ESP32 & GROQ</div>
    </div>
</body>
</html>
)rawhtml";

static void handleRoot();
static void handleSave();

void initCredentials() {
    Preferences prefs;
    prefs.begin("fayasai", true); // Read-only
    s_ssid = prefs.getString("ssid", "");
    s_password = prefs.getString("pass", "");
    s_apiKey = prefs.getString("apikey", "");
    prefs.end();

    // Fall back to defaults in config.h if NVS is empty
    if (s_ssid.length() == 0) {
        s_ssid = WIFI_SSID;
        s_password = WIFI_PASSWORD;
    }
    if (s_apiKey.length() == 0) {
        s_apiKey = AI_API_KEY;
    }
}

String getSSID() { return s_ssid; }
String getPassword() { return s_password; }
String getApiKey() { return s_apiKey; }

bool connect() {
    WiFi.mode(WIFI_STA);
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
    WiFi.begin(s_ssid.c_str(), s_password.c_str());

    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((millis() - startMs) >= WIFI_CONNECT_TIMEOUT_MS) {
            return false;
        }
        delay(100);
    }

    s_lastCheckMs = millis();
    return true;
}

bool isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool reconnectIfNeeded() {
    unsigned long now = millis();
    if ((now - s_lastCheckMs) < WIFI_CHECK_INTERVAL_MS) {
        return false;
    }
    s_lastCheckMs = now;

    if (WiFi.status() == WL_CONNECTED) {
        if (s_reconnecting) {
            s_reconnecting = false;
            return true;
        }
        return false;
    }

    s_reconnecting = true;
    WiFi.disconnect();
    WiFi.begin(s_ssid.c_str(), s_password.c_str());
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

void startConfigPortal(const String& errorMsg) {
    s_portalErrorMsg = errorMsg;
    Serial.println(F("[Portal] Starting Access Point mode..."));
    
    // Shut down STA mode and clear existing connections
    WiFi.disconnect(true);
    delay(100);
    
    WiFi.mode(WIFI_AP);
    // Start open network "FayasAI"
    WiFi.softAP("FayasAI");
    
    Serial.printf("[Portal] Access Point 'FayasAI' active. IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Configure mDNS
    if (MDNS.begin("fayasai")) {
        Serial.println(F("[Portal] mDNS responder active: http://fayasai.local"));
        MDNS.addService("http", "tcp", 80);
    }

    // Set up Web Server routes
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/save", HTTP_POST, handleSave);
    s_server.begin();
    
    Serial.println(F("[Portal] Configuration Web Server listening on port 80"));
}

void handlePortal() {
    s_server.handleClient();
    delay(2); // Yield to prevent system watchdog triggers
}

static void handleRoot() {
    String html = FPSTR(CONFIG_HTML);
    
    // Insert connection error warning box if present
    String statusHtml = "";
    if (s_portalErrorMsg.length() > 0) {
        statusHtml = "<div class=\"status-box\"><strong>Error Status:</strong> " + s_portalErrorMsg + "</div>";
    }
    
    html.replace("%STATUS_PLACEHOLDER%", statusHtml);
    html.replace("%SSID_VAL%", s_ssid);
    html.replace("%PASSWORD_VAL%", s_password);
    html.replace("%APIKEY_VAL%", s_apiKey);
    
    s_server.send(200, "text/html", html);
}

static void handleSave() {
    if (s_server.hasArg("ssid") && s_server.hasArg("apikey")) {
        String newSsid = s_server.arg("ssid");
        String newPass = s_server.arg("password");
        String newApiKey = s_server.arg("apikey");

        // Save credentials to NVS using Preferences
        Preferences prefs;
        prefs.begin("fayasai", false);
        prefs.putString("ssid", newSsid);
        prefs.putString("pass", newPass);
        prefs.putString("apikey", newApiKey);
        prefs.end();

        Serial.println(F("[Portal] Configurations saved. Rebooting..."));

        // Serve confirmation response page
        String confirmation = F(
            "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
            "<title>Configuration Saved</title>"
            "<style>"
            "body{background:#0F0C20;color:#fff;font-family:sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;}"
            ".card{background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);backdrop-filter:blur(20px);border-radius:15px;padding:35px;text-align:center;max-width:400px;width:90%;box-shadow:0 8px 32px rgba(0,0,0,0.3);box-sizing:border-box;}"
            "h3{color:#00D2FC;margin-top:0;font-size:22px;}"
            "p{color:rgba(255,255,255,0.7);font-size:14px;line-height:1.5;}"
            "</style></head><body>"
            "<div class=\"card\">"
            "<h3>Settings Saved!</h3>"
            "<p>Credentials have been securely updated. The device is restarting now to connect to the new Wi-Fi network...</p>"
            "</div></body></html>"
        );
        
        s_server.send(200, "text/html", confirmation);
        delay(1500); // Give the browser time to receive the webpage response
        ESP.restart();
    } else {
        s_server.send(400, "text/plain", "Invalid Data Submitted.");
    }
}

} // namespace FayasWiFi
