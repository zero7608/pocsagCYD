#pragma once
#include <TFT_eSPI.h>
#include "PageStore.h"

// Retro green-on-black phosphor palette
#define C_BG        TFT_BLACK
#define C_GREEN     0x07E0   // bright green
#define C_DIM       0x03E0   // dim green  (older pages)
#define C_AMBER     0xFD20   // amber — newest page highlight
#define C_HEADER    0x07E0
#define C_DIVIDER   0x0380   // very dim green rule
#define C_STATUS_OK 0x07E0
#define C_STATUS_ERR 0xF800  // red

class Display {
public:
    void begin();
    void drawLogo();
    void drawHeader(const char* time_str, float freq_mhz, bool mqtt_ok);
    void drawPages(const PageStore& store, uint8_t scroll_offset);
    void drawStatusBar(bool wifi_ok, bool mqtt_ok, uint8_t unread);
    void flashNew();         // brief amber flash on newest entry

private:
    TFT_eSPI _tft;

    void drawPageEntry(const Page* p, int y, bool newest);
    int  _pageAreaY = 32;
    int  _pageAreaH = 268;   // 320 - 32 header - 20 status bar
};
