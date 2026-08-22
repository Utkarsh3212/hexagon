#pragma once

#include <cstddef>
#include <cstdint>

// `ref` points back to the owner's index and is updated whenever an item moves.
// This keeps heap operations generic while allowing O(log N) arbitrary delete.
struct HeapItem {
    uint64_t val = 0;
    size_t *ref = nullptr;
};

void heap_update(HeapItem *items, size_t pos, size_t len);
