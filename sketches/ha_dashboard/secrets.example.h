// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is listed in .gitignore and is never committed.

#pragma once

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// Base URL of Home Assistant, no trailing slash, e.g. "http://192.168.1.10:8123"
#define HA_URL "http://homeassistant.local:8123"

// Long-lived access token: HA profile -> Security -> Long-lived access tokens -> Create token
#define HA_TOKEN "paste-token-here"
