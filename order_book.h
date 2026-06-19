#pragma once
#include <cstdint>
#include <vector>

// Shared between order_matching.cpp and main.cpp
struct PriceLevel {
    int32_t head_order_idx = -1;
    int32_t tail_order_idx = -1;
};

static constexpr int64_t PRICE_MIN = 1;
static constexpr int64_t PRICE_MAX = 999998;
static constexpr int64_t BOOK_SIZE = 1000000;

// Book state — defined in order_matching.cpp, read by main.cpp for display
extern std::vector<PriceLevel> asks_book;
extern std::vector<PriceLevel> bids_book;
extern int64_t best_bid;
extern int64_t best_ask;
