#include "PageStore.h"

PageStore::PageStore() : _head(0), _count(0), _unread(0) {
    memset(_buf, 0, sizeof(_buf));
}

void PageStore::push(const Page& p) {
    _buf[_head] = p;
    _buf[_head].fresh = true;
    _head = (_head + 1) % MAX_PAGES;
    if (_count < MAX_PAGES) _count++;
    if (_unread < 255) _unread++;
}

const Page* PageStore::get(uint8_t idx) const {
    if (idx >= _count) return nullptr;
    // newest first: slot just before _head, going backwards
    int8_t slot = ((int)_head - 1 - idx + MAX_PAGES) % MAX_PAGES;
    return &_buf[slot];
}
