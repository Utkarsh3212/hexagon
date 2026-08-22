#include "heap.h"

#include <cassert>

namespace {

size_t heap_parent(size_t index) {
    return (index + 1) / 2 - 1;
}

size_t heap_left(size_t index) {
    return index * 2 + 1;
}

size_t heap_right(size_t index) {
    return index * 2 + 2;
}

void heap_up(HeapItem *items, size_t pos) {
    HeapItem moving = items[pos];
    while (pos > 0 && items[heap_parent(pos)].val > moving.val) {
        items[pos] = items[heap_parent(pos)];
        *items[pos].ref = pos;
        pos = heap_parent(pos);
    }
    items[pos] = moving;
    *items[pos].ref = pos;
}

void heap_down(HeapItem *items, size_t pos, size_t len) {
    HeapItem moving = items[pos];
    while (true) {
        const size_t left = heap_left(pos);
        const size_t right = heap_right(pos);
        size_t min_pos = pos;
        uint64_t min_value = moving.val;
        if (left < len && items[left].val < min_value) {
            min_pos = left;
            min_value = items[left].val;
        }
        if (right < len && items[right].val < min_value) {
            min_pos = right;
        }
        if (min_pos == pos) {
            break;
        }
        items[pos] = items[min_pos];
        *items[pos].ref = pos;
        pos = min_pos;
    }
    items[pos] = moving;
    *items[pos].ref = pos;
}

}  // namespace

void heap_update(HeapItem *items, size_t pos, size_t len) {
    assert(items && pos < len && items[pos].ref);
    if (pos > 0 && items[heap_parent(pos)].val > items[pos].val) {
        heap_up(items, pos);
    } else {
        heap_down(items, pos, len);
    }
}
