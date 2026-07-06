# credentials.h

A generated C++ header file containing local sensitive credentials (WiFi SSID, Password, and Groq API Key).

---

## 🔒 Security Warning

> [!WARNING]
> This file is generated dynamically from the root `.env` configuration. It contains sensitive credentials and is explicitly ignored in `.gitignore` to prevent committing secrets to public Git repositories.

---

## ⚙️ Generated Definitions

If compiled after running the generator script, this file defines:
- `WIFI_SSID`: SSID of the Wi-Fi network.
- `WIFI_PASSWORD`: Password of the Wi-Fi network.
- `AI_API_KEY`: Groq API authorization key.

---

## 🚀 Lifecycle Generator

This file is automatically updated or created by running the python helper script in the workspace root:

```bash
python3 generate_credentials.py
```

The script parses `.env`, extracts variables, and writes this header. If you do not have Python installed, you can duplicate `.env.example` as `.env`, fill it in, and manually write this header using the template:

```cpp
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASSWORD   "YOUR_PASSWORD"
#define AI_API_KEY      "YOUR_API_KEY"

#endif
```
