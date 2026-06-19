#pragma once
#include <atomic>
#include <vector>
#include <cstddef>

template<typename T>
class SPSCQueue
{
private:
    // alignas(64) ensures head and tail are on different CPU cache lines.
    // This prevents "False Sharing" across cores.
    alignas(64) std::atomic<size_t> head{ 0 };
    alignas(64) std::atomic<size_t> tail{ 0 };

    size_t capacity;
    std::vector<T> buffer;

public:
    explicit SPSCQueue(size_t size) : capacity(size), buffer(size) {}

    bool push(const T& item)
    {
        const size_t current_tail = tail.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) % capacity;

        if (next_tail == head.load(std::memory_order_acquire)) return false; // Full

        buffer[current_tail] = item;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item)
    {
        const size_t current_head = head.load(std::memory_order_relaxed);

        if (current_head == tail.load(std::memory_order_acquire)) return false; // Empty

        item = buffer[current_head];
        head.store((current_head + 1) % capacity, std::memory_order_release);
        return true;
    }
};
