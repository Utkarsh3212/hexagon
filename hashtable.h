#pragma once

#include <cstddef>
#include <cstdint>

// Intrusive node embedded in each value stored by the table.
struct HNode {
    HNode *next = nullptr;
    uint64_t hcode = 0;
};

struct HTab {
    HNode **tab = nullptr;
    size_t mask = 0;
    size_t size = 0;
};

// Resizing is progressive: lookups consult both tables while a bounded number
// of nodes are moved from `older` to `newer` on each operation.
struct HMap {
    HTab newer;
    HTab older;
    size_t migrate_pos = 0;
};

using HNodeEq = bool (*)(HNode *, HNode *);
using HNodeVisitor = bool (*)(HNode *, void *);

HNode *hm_lookup(HMap *hmap, HNode *key, HNodeEq eq);
void hm_insert(HMap *hmap, HNode *node);
HNode *hm_delete(HMap *hmap, HNode *key, HNodeEq eq);
void hm_clear(HMap *hmap);
size_t hm_size(const HMap *hmap);
void hm_foreach(HMap *hmap, HNodeVisitor visitor, void *arg);
