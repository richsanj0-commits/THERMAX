#pragma once

// Copy this file to secrets.h and fill in your own values.

// Router / laptop hotspot the ESP32 joins (must be 2.4 GHz)
const char* WIFI_SSID = "your-wifi-name";
const char* WIFI_PASS = "your-wifi-password";

// Fallback hotspot the ESP32 creates if the router is out of range
const char* AP_SSID   = "THERMAX-Dashboard";
const char* AP_PASS   = "change-me-8-chars-min";
