#pragma once

#include "avl.h"
#include "hashtable.h"

#include <cstddef>
#include <cstdint>

// A sorted set has one copy of each value and two intrusive indexes:
// a hash table by name and an AVL tree by (score, name).
struct ZSet {
    AVLNode *root = nullptr;
    HMap hmap;
};

struct ZNode {
    AVLNode tree;
    HNode hmap;
    double score = 0;
    size_t len = 0;
    char name[1];
};

bool zset_insert(ZSet *zset, const char *name, size_t len, double score);
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len);
void zset_delete(ZSet *zset, ZNode *node);
ZNode *zset_seekge(ZSet *zset, double score, const char *name, size_t len);
void zset_clear(ZSet *zset);
ZNode *znode_offset(ZNode *node, int64_t offset);
