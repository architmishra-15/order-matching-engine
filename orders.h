#include <cstdint>
#pragma once

typedef enum {
    BUY,
    SELL,
    CANCEL
} Side;

typedef struct {
    uint64_t id;
    uint64_t price;
    uint32_t volume;
    Side side;

    // The secret to cache-friendly memory pooling:
    // This points to the index of the next order in the pre-allocated array.
    int32_t next_order_index;
} FastOrder;
