#!/usr/bin/env python3
"""
Fayas AI — Credentials Header Generator
Reads '.env' file (or falls back to '.env.example') and creates
'credentials.h' so that credentials are kept out of Git.
"""

import os
import re
import sys

def main():
    env_path = '.env'
    example_path = '.env.example'
    output_path = 'credentials.h'

    # Read .env if it exists, otherwise fall back to .env.example
    target_path = env_path if os.path.exists(env_path) else example_path
    
    if not os.path.exists(target_path):
        print(f"Error: Neither {env_path} nor {example_path} was found in the current directory!")
        sys.exit(1)

    ssid = ""
    password = ""
    api_key = ""

    with open(target_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            # Skip empty lines and comment lines
            if not line or line.startswith('#'):
                continue
            # Parse KEY="VALUE" or KEY=VALUE
            match = re.match(r'^([^=]+)=(.*)$', line)
            if match:
                key = match.group(1).strip()
                val = match.group(2).strip().strip('"').strip("'")
                if key == 'WIFI_SSID':
                    ssid = val
                elif key == 'WIFI_PASSWORD':
                    password = val
                elif key == 'AI_API_KEY':
                    api_key = val

    header_content = f"""// Generated credentials file. Do not edit directly or commit to Git!
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#define WIFI_SSID       "{ssid}"
#define WIFI_PASSWORD   "{password}"
#define AI_API_KEY      "{api_key}"

#endif // CREDENTIALS_H
"""

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(header_content)

    print(f"Successfully generated '{output_path}' using values from '{target_path}'.")

if __name__ == '__main__':
    main()
