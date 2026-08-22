#pragma once

#include <cstdint>

// Intrusive AVL node augmented with subtree size. `cnt` makes an offset/rank
// lookup O(log N), regardless of the magnitude of the offset.
struct AVLNode {
    AVLNode *parent = nullptr;
    AVLNode *left = nullptr;
    AVLNode *right = nullptr;
    uint32_t height = 0;
    uint32_t cnt = 0;
};

inline void avl_init(AVLNode *node) {
    node->parent = node->left = node->right = nullptr;
    node->height = 1;
    node->cnt = 1;
}

inline uint32_t avl_height(const AVLNode *node) {
    return node ? node->height : 0;
}

inline uint32_t avl_cnt(const AVLNode *node) {
    return node ? node->cnt : 0;
}

AVLNode *avl_fix(AVLNode *node);
AVLNode *avl_del(AVLNode *node);
AVLNode *avl_offset(AVLNode *node, int64_t offset);
