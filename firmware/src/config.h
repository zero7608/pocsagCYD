#pragma once

// ── Credentials — defined in secrets.h (gitignored, see secrets.h.example) ───
#include "secrets.h"

// ── MQTT (non-sensitive) ──────────────────────────────────────────────────────
#define MQTT_TOPIC    "pocsag/rx"
#define MQTT_CLIENT   "cyd-pocsag-display"

// ── Display ───────────────────────────────────────────────────────────────────
#define MAX_PAGES     10    // number of pages kept in ring buffer
#define MSG_MAX_LEN   120   // max message characters stored

// ── CYD RGB LED (active LOW) ──────────────────────────────────────────────────
#define LED_R  4
#define LED_G  16
#define LED_B  17

#define LED_ON  LOW
#define LED_OFF HIGH
