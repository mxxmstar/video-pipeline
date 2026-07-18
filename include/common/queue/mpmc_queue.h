#pragma once

#include "cameron/concurrentqueue.h"
#include "cameron/blockingconcurrentqueue.h"

#include <cstddef>

template<typename T>
class BoundedMpmcQueue {
public:
    explicit BoundedMpmcQueue(size_t capacity)
        : queue_(capacity), capacity_(capacity) {}

    bool push(const T& item) {
        return queue_.enqueue(item);
    }

    bool push(T&& item) {
        return queue_.enqueue(std::move(item));
    }

    bool pop(T& item) {
        return queue_.try_dequeue(item);
    }

    bool empty() const {
        return queue_.size_approx() == 0;
    }

    size_t size() const {
        return queue_.size_approx();
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    moodycamel::ConcurrentQueue<T> queue_;
    size_t capacity_;
};

template<typename T>
class UnboundedMpmcQueue {
public:
    UnboundedMpmcQueue() = default;

    explicit UnboundedMpmcQueue(size_t initial_capacity)
        : queue_(initial_capacity) {}

    void push(const T& item) {
        queue_.enqueue(item);
    }

    void push(T&& item) {
        queue_.enqueue(std::move(item));
    }

    bool try_pop(T& item) {
        return queue_.try_dequeue(item);
    }

    void wait_pop(T& item) {
        queue_.wait_dequeue(item);
    }

    bool empty() const {
        return queue_.size_approx() == 0;
    }

    size_t size() const {
        return queue_.size_approx();
    }

private:
    moodycamel::BlockingConcurrentQueue<T> queue_;
};