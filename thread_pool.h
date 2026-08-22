#pragma once

#include <pthread.h>

#include <cstddef>
#include <deque>
#include <vector>

struct Work {
    void (*function)(void *) = nullptr;
    void *argument = nullptr;
};

struct ThreadPool {
    std::vector<pthread_t> threads;
    std::deque<Work> queue;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    bool stopping = false;
};

void thread_pool_init(ThreadPool *pool, size_t thread_count);
void thread_pool_queue(ThreadPool *pool, void (*function)(void *), void *argument);
void thread_pool_destroy(ThreadPool *pool);
