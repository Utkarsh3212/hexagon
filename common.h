#pragma once

#include <cstddef>
#include <cstdint>

// Recover the enclosing object from one of its intrusive data-structure nodes.
// This preserves intrusive ownership recovery without requiring GNU statement
// expressions, so the project builds cleanly in standard C++ mode.
#define container_of(ptr, type, member)                                  \
    reinterpret_cast<type *>(reinterpret_cast<unsigned char *>(ptr) -   \
                             offsetof(type, member))

// A small, non-cryptographic hash suitable for hash-table indexing.
inline uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t hash = 0x811C9DC5;
    for (size_t i = 0; i < len; ++i) {
        hash = (hash + data[i]) * 0x01000193;
    }
    return hash;
}
