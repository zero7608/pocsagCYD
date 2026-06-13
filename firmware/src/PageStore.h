#pragma once
#include <Arduino.h>
#include "config.h"

struct Page {
    char    capcode[12];
    char    msg[MSG_MAX_LEN + 1];
    char    time_str[9];    // "HH:MM:SS"
    float   freq_mhz;
    uint16_t baud;
    bool    is_alpha;
    bool    fresh;          // true until first render
};

class PageStore {
public:
    PageStore();
    void        push(const Page& p);
    const Page* get(uint8_t idx) const;  // 0 = newest
    uint8_t     count() const { return _count; }
    uint8_t     unread() const { return _unread; }
    void        markRead() { _unread = 0; }

private:
    Page    _buf[MAX_PAGES];
    uint8_t _head;   // index of next write slot
    uint8_t _count;
    uint8_t _unread;
};
