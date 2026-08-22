#include "zset.h"

#include "common.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

ZNode *znode_new(const char *name, size_t len, double score) {
    // `name[1]` is the portable struct-hack spelling; the trailing allocation
    // stores the rest of the bytes inline with the node.
    const size_t extra = len > 0 ? len - 1 : 0;
    void *storage = std::malloc(sizeof(ZNode) + extra);
    if (!storage) {
        std::abort();
    }
    ZNode *node = new (storage) ZNode();
    avl_init(&node->tree);
    node->hmap.next = nullptr;
    node->hmap.hcode = str_hash(reinterpret_cast<const uint8_t *>(name), len);
    node->score = score;
    node->len = len;
    if (len) {
        std::memcpy(node->name, name, len);
    }
    return node;
}

void znode_del(ZNode *node) {
    node->~ZNode();
    std::free(node);
}

bool zless(AVLNode *lhs, double score, const char *name, size_t len) {
    ZNode *left = container_of(lhs, ZNode, tree);
    if (left->score != score) {
        return left->score < score;
    }
    const size_t common = std::min(left->len, len);
    const int comparison = std::memcmp(left->name, name, common);
    return comparison != 0 ? comparison < 0 : left->len < len;
}

bool zless(AVLNode *lhs, AVLNode *rhs) {
    ZNode *right = container_of(rhs, ZNode, tree);
    return zless(lhs, right->score, right->name, right->len);
}

void tree_insert(ZSet *zset, ZNode *node) {
    AVLNode *parent = nullptr;
    AVLNode **from = &zset->root;
    while (*from) {
        parent = *from;
        from = zless(&node->tree, parent) ? &parent->left : &parent->right;
    }
    *from = &node->tree;
    node->tree.parent = parent;
    zset->root = avl_fix(&node->tree);
}

void zset_update(ZSet *zset, ZNode *node, double score) {
    if (node->score == score) {
        return;
    }
    zset->root = avl_del(&node->tree);
    avl_init(&node->tree);
    node->score = score;
    tree_insert(zset, node);
}

struct HKey {
    HNode node;
    const char *name = nullptr;
    size_t len = 0;
};

bool hcmp(HNode *node, HNode *key) {
    ZNode *znode = container_of(node, ZNode, hmap);
    HKey *hkey = container_of(key, HKey, node);
    return znode->len == hkey->len &&
           std::memcmp(znode->name, hkey->name, znode->len) == 0;
}

void tree_dispose(AVLNode *node) {
    if (!node) {
        return;
    }
    tree_dispose(node->left);
    tree_dispose(node->right);
    znode_del(container_of(node, ZNode, tree));
}

}  // namespace

bool zset_insert(ZSet *zset, const char *name, size_t len, double score) {
    ZNode *node = zset_lookup(zset, name, len);
    if (node) {
        zset_update(zset, node, score);
        return false;
    }

    node = znode_new(name, len, score);
    hm_insert(&zset->hmap, &node->hmap);
    tree_insert(zset, node);
    return true;
}

ZNode *zset_lookup(ZSet *zset, const char *name, size_t len) {
    if (!zset->root) {
        return nullptr;
    }
    HKey key;
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(name), len);
    key.name = name;
    key.len = len;
    HNode *found = hm_lookup(&zset->hmap, &key.node, &hcmp);
    return found ? container_of(found, ZNode, hmap) : nullptr;
}

void zset_delete(ZSet *zset, ZNode *node) {
    HKey key;
    key.node.hcode = node->hmap.hcode;
    key.name = node->name;
    key.len = node->len;
    HNode *found = hm_delete(&zset->hmap, &key.node, &hcmp);
    assert(found == &node->hmap);

    zset->root = avl_del(&node->tree);
    znode_del(node);
}

ZNode *zset_seekge(ZSet *zset, double score, const char *name, size_t len) {
    AVLNode *found = nullptr;
    for (AVLNode *node = zset->root; node;) {
        if (zless(node, score, name, len)) {
            node = node->right;
        } else {
            found = node;
            node = node->left;
        }
    }
    return found ? container_of(found, ZNode, tree) : nullptr;
}

void zset_clear(ZSet *zset) {
    hm_clear(&zset->hmap);
    tree_dispose(zset->root);
    zset->root = nullptr;
}

ZNode *znode_offset(ZNode *node, int64_t offset) {
    AVLNode *tree_node = node ? avl_offset(&node->tree, offset) : nullptr;
    return tree_node ? container_of(tree_node, ZNode, tree) : nullptr;
}
