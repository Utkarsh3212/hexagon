#pragma once

// Intrusive circular doubly-linked list. A sentinel node represents the list,
// eliminating empty-list special cases for insertion and removal.
struct DList {
    DList *prev = nullptr;
    DList *next = nullptr;
};

inline void dlist_init(DList *node) {
    node->prev = node->next = node;
}

inline bool dlist_empty(const DList *node) {
    return node->next == node;
}

inline void dlist_detach(DList *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = node;
}

inline void dlist_insert_before(DList *target, DList *node) {
    DList *previous = target->prev;
    previous->next = node;
    node->prev = previous;
    node->next = target;
    target->prev = node;
}
