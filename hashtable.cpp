#include "hashtable.h"

#include <cassert>
#include <cstdlib>

namespace {

void h_init(HTab *htab, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);
    htab->tab = static_cast<HNode **>(std::calloc(n, sizeof(HNode *)));
    if (!htab->tab) {
        std::abort();
    }
    htab->mask = n - 1;
    htab->size = 0;
}

void h_insert(HTab *htab, HNode *node) {
    const size_t pos = node->hcode & htab->mask;
    node->next = htab->tab[pos];
    htab->tab[pos] = node;
    ++htab->size;
}

// Return the address of the pointer which owns the matching node. This makes
// deletion identical for a bucket head and a node in the middle of a chain.
HNode **h_lookup(HTab *htab, HNode *key, HNodeEq eq) {
    if (!htab->tab) {
        return nullptr;
    }

    const size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    for (HNode *cur; (cur = *from) != nullptr; from = &cur->next) {
        if (cur->hcode == key->hcode && eq(cur, key)) {
            return from;
        }
    }
    return nullptr;
}

HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = node->next;
    node->next = nullptr;
    --htab->size;
    return node;
}

constexpr size_t kRehashingWork = 128;
constexpr size_t kMaxLoadFactor = 8;

void hm_help_rehashing(HMap *hmap) {
    size_t nwork = 0;
    while (nwork < kRehashingWork && hmap->older.size > 0) {
        assert(hmap->older.tab && hmap->migrate_pos <= hmap->older.mask);
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from) {
            ++hmap->migrate_pos;
            continue;
        }
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        ++nwork;
    }

    if (hmap->older.size == 0 && hmap->older.tab) {
        std::free(hmap->older.tab);
        hmap->older = HTab{};
        hmap->migrate_pos = 0;
    }
}

void hm_trigger_rehashing(HMap *hmap) {
    assert(!hmap->older.tab);
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

bool h_foreach(HTab *htab, HNodeVisitor visitor, void *arg) {
    if (!htab->tab) {
        return true;
    }
    for (size_t i = 0; i <= htab->mask; ++i) {
        for (HNode *node = htab->tab[i]; node; node = node->next) {
            if (!visitor(node, arg)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

HNode *hm_lookup(HMap *hmap, HNode *key, HNodeEq eq) {
    hm_help_rehashing(hmap);
    HNode **from = h_lookup(&hmap->newer, key, eq);
    if (!from) {
        from = h_lookup(&hmap->older, key, eq);
    }
    return from ? *from : nullptr;
}

void hm_insert(HMap *hmap, HNode *node) {
    if (!hmap->newer.tab) {
        h_init(&hmap->newer, 4);
    }
    h_insert(&hmap->newer, node);

    if (!hmap->older.tab) {
        const size_t threshold = (hmap->newer.mask + 1) * kMaxLoadFactor;
        if (hmap->newer.size >= threshold) {
            hm_trigger_rehashing(hmap);
        }
    }
    hm_help_rehashing(hmap);
}

HNode *hm_delete(HMap *hmap, HNode *key, HNodeEq eq) {
    hm_help_rehashing(hmap);
    if (HNode **from = h_lookup(&hmap->newer, key, eq)) {
        return h_detach(&hmap->newer, from);
    }
    if (HNode **from = h_lookup(&hmap->older, key, eq)) {
        return h_detach(&hmap->older, from);
    }
    return nullptr;
}

void hm_clear(HMap *hmap) {
    std::free(hmap->newer.tab);
    std::free(hmap->older.tab);
    *hmap = HMap{};
}

size_t hm_size(const HMap *hmap) {
    return hmap->newer.size + hmap->older.size;
}

void hm_foreach(HMap *hmap, HNodeVisitor visitor, void *arg) {
    if (h_foreach(&hmap->newer, visitor, arg)) {
        h_foreach(&hmap->older, visitor, arg);
    }
}
