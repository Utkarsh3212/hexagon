#include "avl.h"

#include <cassert>
#include <algorithm>

namespace {

void avl_update(AVLNode *node) {
    node->height = 1 + std::max(avl_height(node->left), avl_height(node->right));
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

AVLNode *rot_left(AVLNode *node) {
    AVLNode *parent = node->parent;
    AVLNode *new_node = node->right;
    assert(new_node);
    AVLNode *inner = new_node->left;

    node->right = inner;
    if (inner) {
        inner->parent = node;
    }
    new_node->parent = parent;
    new_node->left = node;
    node->parent = new_node;

    avl_update(node);
    avl_update(new_node);
    return new_node;
}

AVLNode *rot_right(AVLNode *node) {
    AVLNode *parent = node->parent;
    AVLNode *new_node = node->left;
    assert(new_node);
    AVLNode *inner = new_node->right;

    node->left = inner;
    if (inner) {
        inner->parent = node;
    }
    new_node->parent = parent;
    new_node->right = node;
    node->parent = new_node;

    avl_update(node);
    avl_update(new_node);
    return new_node;
}

AVLNode *avl_fix_left(AVLNode *node) {
    if (avl_height(node->left->left) < avl_height(node->left->right)) {
        node->left = rot_left(node->left);
    }
    return rot_right(node);
}

AVLNode *avl_fix_right(AVLNode *node) {
    if (avl_height(node->right->right) < avl_height(node->right->left)) {
        node->right = rot_right(node->right);
    }
    return rot_left(node);
}

// Detach a node which has at most one child.
AVLNode *avl_del_easy(AVLNode *node) {
    assert(!node->left || !node->right);
    AVLNode *child = node->left ? node->left : node->right;
    AVLNode *parent = node->parent;
    if (child) {
        child->parent = parent;
    }
    if (!parent) {
        return child;
    }
    AVLNode **from = parent->left == node ? &parent->left : &parent->right;
    *from = child;
    return avl_fix(parent);
}

}  // namespace

AVLNode *avl_fix(AVLNode *node) {
    while (true) {
        AVLNode **from = &node;
        AVLNode *parent = node->parent;
        if (parent) {
            from = parent->left == node ? &parent->left : &parent->right;
        }

        avl_update(node);
        const uint32_t left = avl_height(node->left);
        const uint32_t right = avl_height(node->right);
        if (left == right + 2) {
            *from = avl_fix_left(node);
        } else if (left + 2 == right) {
            *from = avl_fix_right(node);
        }

        if (!parent) {
            return *from;
        }
        node = parent;
    }
}

AVLNode *avl_del(AVLNode *node) {
    if (!node->left || !node->right) {
        return avl_del_easy(node);
    }

    AVLNode *successor = node->right;
    while (successor->left) {
        successor = successor->left;
    }
    AVLNode *root = avl_del_easy(successor);

    *successor = *node;
    if (successor->left) {
        successor->left->parent = successor;
    }
    if (successor->right) {
        successor->right->parent = successor;
    }

    AVLNode **from = &root;
    AVLNode *parent = node->parent;
    if (parent) {
        from = parent->left == node ? &parent->left : &parent->right;
    }
    *from = successor;
    return root;
}

AVLNode *avl_offset(AVLNode *node, int64_t offset) {
    if (!node) {
        return nullptr;
    }

    int64_t pos = 0;
    while (offset != pos) {
        if (pos < offset && pos + avl_cnt(node->right) >= offset) {
            node = node->right;
            pos += avl_cnt(node->left) + 1;
        } else if (pos > offset && pos - avl_cnt(node->left) <= offset) {
            node = node->left;
            pos -= avl_cnt(node->right) + 1;
        } else {
            AVLNode *parent = node->parent;
            if (!parent) {
                return nullptr;
            }
            if (parent->right == node) {
                pos -= avl_cnt(node->left) + 1;
            } else {
                pos += avl_cnt(node->right) + 1;
            }
            node = parent;
        }
    }
    return node;
}
