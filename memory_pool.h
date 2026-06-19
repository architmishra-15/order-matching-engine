#pragma once

#include <vector>
#include "orders.h"

// O(1) allocation and deallocation
class OrderPool {
private:
    std::vector<FastOrder> pool;
    int32_t free_head;

public:
    OrderPool(size_t capacity) {
        pool.resize(capacity);

        for (size_t i = 0; i < capacity; ++i) {
            pool[i].next_order_index = i + 1;
        }

        pool[capacity - 1].next_order_index = -1;
        free_head = 0;
    }

    int32_t allocate() {
        if (free_head == -1) return -1;
        int32_t index = free_head;
        free_head = pool[index].next_order_index;
        pool[index].next_order_index = -1;
        return index;
    }

    void free(int32_t index) {
        pool[index].next_order_index = free_head;
        free_head = index;
    }

    void reset() {
        for (size_t i = 0; i < pool.size(); ++i)
            pool[i].next_order_index = (int32_t)(i + 1);
        pool.back().next_order_index = -1;
        free_head = 0;
    }

    FastOrder& get(int32_t index) { return pool[index]; };
};
