#include "Display.h"

// Row height for each page entry (time + 2 message lines)
static const int ROW_H = 52;

void Display::begin() {
    _tft.init();
    _tft.setRotation(0);   // portrait
    _tft.fillScreen(C_BG);
    _tft.setTextDatum(TL_DATUM);
    ledcAttach(TFT_BL, 5000, 8);
    ledcWrite(TFT_BL, 220);
}

void Display::drawHeader(const char* time_str, float freq_mhz, bool mqtt_ok) {
    _tft.fillRect(0, 0, 240, 32, C_GREEN);
    _tft.setTextColor(C_BG, C_GREEN);
    _tft.setTextSize(1);
    _tft.setTextFont(4);   // 26px

    // Left: label
    _tft.drawString("POCSAG-RX", 4, 4);

    // Right: clock
    _tft.setTextDatum(TR_DATUM);
    _tft.drawString(time_str, 236, 4);
    _tft.setTextDatum(TL_DATUM);

    // Thin freq + status line just below header
    _tft.fillRect(0, 32, 240, 14, 0x0200);  // very dark green
    _tft.setTextFont(1);
    _tft.setTextSize(1);
    _tft.setTextColor(C_DIM, 0x0200);
    char buf[32];
    snprintf(buf, sizeof(buf), " %.4f MHz", freq_mhz);
    _tft.drawString(buf, 0, 33);

    const char* dot = mqtt_ok ? "\x07 MQTT" : "! MQTT";
    uint16_t dc = mqtt_ok ? C_STATUS_OK : C_STATUS_ERR;
    _tft.setTextColor(dc, 0x0200);
    _tft.setTextDatum(TR_DATUM);
    _tft.drawString(dot, 238, 33);
    _tft.setTextDatum(TL_DATUM);

    _pageAreaY = 46;
    _pageAreaH = 254;  // 320 - 46 - 20
}

void Display::drawPageEntry(TFT_eSprite& spr, const Page* p, int y, bool newest) {
    uint16_t tc = newest ? C_AMBER : C_DIM;
    uint16_t mc = newest ? C_GREEN : C_DIM;

    // Divider rule
    spr.drawFastHLine(0, y, 240, C_DIVIDER);
    y += 2;

    // Time + capcode line
    spr.setTextFont(1);
    spr.setTextSize(1);
    spr.setTextColor(tc, C_BG);
    char buf[32];
    snprintf(buf, sizeof(buf), "%s  #%s", p->time_str, p->capcode);
    spr.drawString(buf, 2, y);

    // Baud badge (right-aligned)
    snprintf(buf, sizeof(buf), "%u", p->baud);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(C_DIVIDER, C_BG);
    spr.drawString(buf, 238, y);
    spr.setTextDatum(TL_DATUM);

    y += 12;

    // Message — wrap at ~28 chars per line, 2 lines max
    spr.setTextColor(mc, C_BG);
    String m(p->msg);
    if (m.length() > 28) {
        // Find a space near char 28 to break on
        int brk = 28;
        for (int i = 28; i > 20; i--) {
            if (m[i] == ' ') { brk = i; break; }
        }
        spr.drawString(m.substring(0, brk), 2, y);
        y += 13;
        String rest = m.substring(brk + 1);
        if (rest.length() > 28) rest = rest.substring(0, 27) + "~";
        spr.drawString(rest, 2, y);
    } else {
        spr.drawString(m, 2, y);
    }
}

void Display::drawPages(const PageStore& store, uint8_t scroll_offset) {
    _spr.createSprite(240, _pageAreaH);
    _spr.fillSprite(C_BG);
    _spr.setTextDatum(TL_DATUM);

    int y = 0;
    uint8_t drawn = 0;
    uint8_t n = store.count();

    for (uint8_t i = scroll_offset; i < n && y < _pageAreaH; i++) {
        const Page* p = store.get(i);
        if (!p) break;
        drawPageEntry(_spr, p, y, (i == 0 && scroll_offset == 0));
        y += ROW_H;
        drawn++;
    }

    if (drawn == 0) {
        _spr.setTextFont(2);
        _spr.setTextColor(C_DIVIDER, C_BG);
        _spr.drawString("waiting for pages...", 20, _pageAreaH / 2 - 8);
    }

    _spr.pushSprite(0, _pageAreaY);
    _spr.deleteSprite();
}

void Display::drawStatusBar(bool wifi_ok, bool mqtt_ok, uint8_t unread) {
    int y = 300;
    _tft.fillRect(0, y, 240, 20, 0x0180);
    _tft.setTextFont(1);
    _tft.setTextSize(1);

    auto dot = [&](const char* label, bool ok, int x) {
        _tft.setTextColor(ok ? C_STATUS_OK : C_STATUS_ERR, 0x0180);
        _tft.drawString(label, x, y + 4);
    };

    dot("WiFi", wifi_ok, 4);
    dot("MQTT", mqtt_ok, 44);

    if (unread > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "+%u new", unread);
        _tft.setTextColor(C_AMBER, 0x0180);
        _tft.setTextDatum(TR_DATUM);
        _tft.drawString(buf, 236, y + 4);
        _tft.setTextDatum(TL_DATUM);
    }
}

void Display::flashNew() {
    // Quick amber flash at top of page area to signal new message
    for (int i = 0; i < 2; i++) {
        _tft.fillRect(0, _pageAreaY, 240, 3, C_AMBER);
        delay(80);
        _tft.fillRect(0, _pageAreaY, 240, 3, C_BG);
        delay(80);
    }
}
