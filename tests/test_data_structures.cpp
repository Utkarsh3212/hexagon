#include "avl.h"
#include "common.h"
#include "hashtable.h"
#include "heap.h"
#include "thread_pool.h"
#include "zset.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

struct HashValue {
    HNode node;
    std::string key;
};

bool hash_value_equal(HNode *node, HNode *key) {
    return container_of(node, HashValue, node)->key ==
           container_of(key, HashValue, node)->key;
}

bool count_node(HNode *, void *argument) {
    ++*static_cast<size_t *>(argument);
    return true;
}

void test_hashtable() {
    HMap map;
    std::vector<std::unique_ptr<HashValue>> values;
    constexpr size_t count = 5000;
    values.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto value = std::make_unique<HashValue>();
        value->key = "key-" + std::to_string(i);
        value->node.hcode = str_hash(
            reinterpret_cast<const uint8_t *>(value->key.data()),
            value->key.size());
        hm_insert(&map, &value->node);
        values.push_back(std::move(value));
    }
    assert(hm_size(&map) == count);

    for (const auto &value : values) {
        HashValue key;
        key.key = value->key;
        key.node.hcode = value->node.hcode;
        assert(hm_lookup(&map, &key.node, &hash_value_equal) == &value->node);
    }

    size_t visited = 0;
    hm_foreach(&map, &count_node, &visited);
    assert(visited == count);

    for (size_t i = 0; i < count; i += 2) {
        HashValue key;
        key.key = values[i]->key;
        key.node.hcode = values[i]->node.hcode;
        assert(hm_delete(&map, &key.node, &hash_value_equal) ==
               &values[i]->node);
    }
    assert(hm_size(&map) == count / 2);
    hm_clear(&map);
}

struct TreeValue {
    AVLNode node;
    uint32_t value = 0;
};

struct Tree {
    AVLNode *root = nullptr;
};

TreeValue *tree_value(AVLNode *node) {
    return container_of(node, TreeValue, node);
}

void tree_insert(Tree &tree, TreeValue *value) {
    avl_init(&value->node);
    AVLNode *parent = nullptr;
    AVLNode **from = &tree.root;
    while (*from) {
        parent = *from;
        from = value->value < tree_value(parent)->value ? &parent->left
                                                        : &parent->right;
    }
    *from = &value->node;
    value->node.parent = parent;
    tree.root = avl_fix(&value->node);
}

uint32_t verify_tree(AVLNode *node, AVLNode *parent,
                     std::multiset<uint32_t> &ordered) {
    if (!node) {
        return 0;
    }
    assert(node->parent == parent);
    const uint32_t left_height = verify_tree(node->left, node, ordered);
    ordered.insert(tree_value(node)->value);
    const uint32_t right_height = verify_tree(node->right, node, ordered);
    assert(left_height <= right_height + 1);
    assert(right_height <= left_height + 1);
    assert(node->height == 1 + std::max(left_height, right_height));
    assert(node->cnt == 1 + avl_cnt(node->left) + avl_cnt(node->right));
    return node->height;
}

void test_avl() {
    Tree tree;
    std::vector<std::unique_ptr<TreeValue>> values;
    constexpr uint32_t count = 1000;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto value = std::make_unique<TreeValue>();
        value->value = i;
        tree_insert(tree, value.get());
        values.push_back(std::move(value));
    }

    std::multiset<uint32_t> ordered;
    verify_tree(tree.root, nullptr, ordered);
    assert(ordered.size() == count);
    AVLNode *minimum = tree.root;
    while (minimum->left) {
        minimum = minimum->left;
    }
    for (uint32_t i = 0; i < count; ++i) {
        assert(tree_value(avl_offset(minimum, i))->value == i);
    }
    assert(!avl_offset(minimum, -1));
    assert(!avl_offset(minimum, count));

    for (uint32_t i = 0; i < count; i += 3) {
        tree.root = avl_del(&values[i]->node);
        values[i].reset();
    }
    ordered.clear();
    verify_tree(tree.root, nullptr, ordered);
    assert(ordered.size() == count - (count + 2) / 3);

    for (auto &value : values) {
        if (value) {
            tree.root = avl_del(&value->node);
            value.reset();
        }
    }
    assert(!tree.root);
}

void test_sorted_set() {
    ZSet set;
    assert(zset_insert(&set, "alice", 5, 10));
    assert(zset_insert(&set, "bob", 3, 20));
    assert(zset_insert(&set, "carol", 5, 20));
    assert(!zset_insert(&set, "alice", 5, 30));
    assert(hm_size(&set.hmap) == 3);
    assert(zset_lookup(&set, "alice", 5)->score == 30);

    ZNode *first = zset_seekge(&set, 20, "", 0);
    assert(first && std::string(first->name, first->len) == "bob");
    ZNode *second = znode_offset(first, 1);
    assert(second && std::string(second->name, second->len) == "carol");
    ZNode *third = znode_offset(second, 1);
    assert(third && std::string(third->name, third->len) == "alice");
    assert(znode_offset(third, -2) == first);

    zset_delete(&set, second);
    assert(!zset_lookup(&set, "carol", 5));
    assert(hm_size(&set.hmap) == 2);
    zset_clear(&set);
    assert(!set.root && hm_size(&set.hmap) == 0);
}

struct HeapOwner {
    size_t index = static_cast<size_t>(-1);
};

void verify_heap(const std::vector<HeapItem> &heap) {
    for (size_t i = 0; i < heap.size(); ++i) {
        const size_t left = i * 2 + 1;
        const size_t right = i * 2 + 2;
        assert(left >= heap.size() || heap[i].val <= heap[left].val);
        assert(right >= heap.size() || heap[i].val <= heap[right].val);
        assert(*heap[i].ref == i);
    }
}

void test_heap() {
    std::vector<std::unique_ptr<HeapOwner>> owners;
    std::vector<HeapItem> heap;
    for (uint64_t value : {50, 20, 90, 10, 70, 30, 40}) {
        auto owner = std::make_unique<HeapOwner>();
        heap.push_back(HeapItem{value, &owner->index});
        heap_update(heap.data(), heap.size() - 1, heap.size());
        owners.push_back(std::move(owner));
        verify_heap(heap);
    }
    assert(heap.front().val == 10);

    const size_t remove_position = owners[1]->index;
    heap[remove_position] = heap.back();
    heap.pop_back();
    if (remove_position < heap.size()) {
        heap_update(heap.data(), remove_position, heap.size());
    }
    verify_heap(heap);
}

struct PoolTest {
    std::atomic<size_t> completed{0};
    std::mutex mutex;
    std::condition_variable condition;
};

void complete_work(void *argument) {
    PoolTest *test = static_cast<PoolTest *>(argument);
    ++test->completed;
    test->condition.notify_all();
}

void test_thread_pool() {
    ThreadPool pool;
    thread_pool_init(&pool, 4);
    PoolTest test;
    constexpr size_t work_count = 200;
    for (size_t i = 0; i < work_count; ++i) {
        thread_pool_queue(&pool, &complete_work, &test);
    }

    std::unique_lock<std::mutex> lock(test.mutex);
    assert(test.condition.wait_for(lock, std::chrono::seconds(3), [&] {
        return test.completed.load() == work_count;
    }));
    lock.unlock();
    thread_pool_destroy(&pool);
}

}  // namespace

int main() {
    test_hashtable();
    test_avl();
    test_sorted_set();
    test_heap();
    test_thread_pool();
    std::puts("data-structure tests passed");
    return 0;
}
