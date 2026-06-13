#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"
#include "PageStore.h"
#include "Display.h"

// ── Globals ───────────────────────────────────────────────────────────────────
PageStore store;
Display   disp;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

volatile bool newPage     = false;
float         currentFreq = 0.0f;
uint8_t       scrollOffset = 0;

// XPT2046 touch (separate SPI bus)
#include <SPI.h>
static SPIClass touchSPI(VSPI);
static const int T_CS  = 33;
static const int T_CLK = 25;
static const int T_DIN = 32;
static const int T_OUT = 39;
static const int T_IRQ = 36;

// ── LED helpers ───────────────────────────────────────────────────────────────
inline void ledOff() {
    digitalWrite(LED_R, LED_OFF);
    digitalWrite(LED_G, LED_OFF);
    digitalWrite(LED_B, LED_OFF);
}

inline void ledColor(bool r, bool g, bool b) {
    digitalWrite(LED_R, r ? LED_ON : LED_OFF);
    digitalWrite(LED_G, g ? LED_ON : LED_OFF);
    digitalWrite(LED_B, b ? LED_ON : LED_OFF);
}

// ── MQTT callback ─────────────────────────────────────────────────────────────
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) return;

    Page p{};
    strlcpy(p.capcode,  doc["capcode"] | "?",     sizeof(p.capcode));
    strlcpy(p.msg,      doc["msg"]     | "",       sizeof(p.msg));
    p.freq_mhz  = doc["freq_mhz"] | 0.0f;
    p.baud      = doc["baud"]     | 0;
    p.is_alpha  = (strcmp(doc["type"] | "alpha", "alpha") == 0);

    // Convert Unix timestamp to local time string
    time_t ts = doc["ts"] | (time_t)0;
    if (ts == 0) ts = time(nullptr);
    struct tm* ti = localtime(&ts);
    snprintf(p.time_str, sizeof(p.time_str), "%02d:%02d:%02d",
             ti->tm_hour, ti->tm_min, ti->tm_sec);

    currentFreq = p.freq_mhz;
    store.push(p);
    scrollOffset = 0;   // jump to top on new page
    newPage = true;

    // Flash amber LED
    ledColor(true, false, false);  // brief red flash (active low means this is red)
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        configTime(-5 * 3600, 3600, "pool.ntp.org");  // ET (UTC-5, DST+1)
        ledColor(false, true, false);  // green = connected
        delay(300);
        ledOff();
    }
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
void connectMqtt() {
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(512);

    while (!mqtt.connected()) {
        if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
            mqtt.subscribe(MQTT_TOPIC, 0);
        } else {
            delay(3000);
        }
    }
}

// ── Touch (simple tap detection) ─────────────────────────────────────────────
static unsigned long lastTouch = 0;

bool isTouched() {
    return digitalRead(T_IRQ) == LOW;
}

// Returns +1 for tap upper half (scroll up), -1 lower half (scroll down), 0 none
int readTouch() {
    if (!isTouched()) return 0;
    if (millis() - lastTouch < 400) return 0;  // debounce
    lastTouch = millis();

    // Raw SPI read of Y coordinate
    touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(T_CS, LOW);
    touchSPI.transfer(0x91);  // read Y
    uint16_t y = (touchSPI.transfer(0) << 8) | touchSPI.transfer(0);
    digitalWrite(T_CS, HIGH);
    touchSPI.endTransaction();

    y >>= 3;  // 12-bit result
    // Y < 2000 → top half → scroll up (show newer)
    return (y < 2000) ? -1 : 1;
}

// ── Time string ───────────────────────────────────────────────────────────────
void getTimeStr(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    snprintf(buf, len, "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // LED
    pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
    ledOff();

    // Touch CS
    pinMode(T_CS, OUTPUT); digitalWrite(T_CS, HIGH);
    pinMode(T_IRQ, INPUT);
    touchSPI.begin(T_CLK, T_OUT, T_DIN, T_CS);

    disp.begin();

    // Show boot screen
    char buf[24];
    snprintf(buf, sizeof(buf), "--:--:--");
    disp.drawHeader(buf, 0.0, false);
    disp.drawPages(store, 0);
    disp.drawStatusBar(false, false, 0);

    connectWifi();
    connectMqtt();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // Reconnect guards
    if (WiFi.status() != WL_CONNECTED) {
        connectWifi();
    }
    if (!mqtt.connected()) {
        connectMqtt();
    }
    mqtt.loop();

    // Touch scroll
    int td = readTouch();
    if (td != 0) {
        int ns = (int)scrollOffset + td;
        if (ns < 0) ns = 0;
        if (ns >= store.count()) ns = store.count() > 0 ? store.count() - 1 : 0;
        scrollOffset = (uint8_t)ns;
        store.markRead();
        newPage = true;  // trigger redraw
    }

    // Redraw on new page or once per second for clock
    static unsigned long lastDraw = 0;
    bool tick = (millis() - lastDraw >= 1000);

    if (newPage || tick) {
        char tstr[9];
        getTimeStr(tstr, sizeof(tstr));

        bool wok = (WiFi.status() == WL_CONNECTED);
        bool mok = mqtt.connected();

        disp.drawHeader(tstr, currentFreq, mok);
        disp.drawPages(store, scrollOffset);
        disp.drawStatusBar(wok, mok, store.unread());

        if (newPage) {
            disp.flashNew();
            ledColor(false, true, false);  // green pulse
            delay(200);
            ledOff();
            store.markRead();
            newPage = false;
        }

        lastDraw = millis();
    }
}
