#include "thread_pool.h"

#include <cassert>

namespace {

void *worker(void *argument) {
    ThreadPool *pool = static_cast<ThreadPool *>(argument);
    while (true) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->queue.empty() && !pool->stopping) {
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }
        if (pool->queue.empty() && pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            return nullptr;
        }

        Work work = pool->queue.front();
        pool->queue.pop_front();
        pthread_mutex_unlock(&pool->mutex);
        work.function(work.argument);
    }
}

}  // namespace

void thread_pool_init(ThreadPool *pool, size_t thread_count) {
    assert(thread_count > 0);
    pool->stopping = false;
    assert(pthread_mutex_init(&pool->mutex, nullptr) == 0);
    assert(pthread_cond_init(&pool->not_empty, nullptr) == 0);

    pool->threads.resize(thread_count);
    for (pthread_t &thread : pool->threads) {
        assert(pthread_create(&thread, nullptr, &worker, pool) == 0);
    }
}

void thread_pool_queue(ThreadPool *pool, void (*function)(void *), void *argument) {
    assert(function);
    pthread_mutex_lock(&pool->mutex);
    assert(!pool->stopping);
    pool->queue.push_back(Work{function, argument});
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
}

void thread_pool_destroy(ThreadPool *pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);

    for (pthread_t thread : pool->threads) {
        pthread_join(thread, nullptr);
    }
    pool->threads.clear();
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);
}
