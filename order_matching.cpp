// order_matching.cpp
#include "orders.h"
#include "memory_pool.h"
#include "order_book.h"
#include <cstdint>
#include <vector>
#include <algorithm>

// ── Book state (definitions live here, declared extern in order_book.h) ───────
std::vector<PriceLevel> asks_book(BOOK_SIZE);
std::vector<PriceLevel> bids_book(BOOK_SIZE);

int64_t best_bid = -1;
int64_t best_ask = BOOK_SIZE; // sentinel: no resting ask exists

// ── Helpers ───────────────────────────────────────────────────────────────────

inline int64_t safe_price(uint64_t raw) {
    if (raw < (uint64_t)PRICE_MIN) return PRICE_MIN;
    if (raw > (uint64_t)PRICE_MAX) return PRICE_MAX;
    return (int64_t)raw;
}

void add_to_book(std::vector<PriceLevel>& book, int32_t order_idx,
                 int64_t price, OrderPool& pool)
{
    PriceLevel& level = book[price];
    if (level.head_order_idx == -1) {
        level.head_order_idx = order_idx;
        level.tail_order_idx = order_idx;
    } else {
        pool.get(level.tail_order_idx).next_order_index = order_idx;
        level.tail_order_idx = order_idx;
    }
}

void cancel_order(std::vector<PriceLevel>& book, uint64_t target_id,
                  int64_t price, OrderPool& pool)
{
    PriceLevel& level   = book[price];
    int32_t current_idx = level.head_order_idx;
    int32_t prev_idx    = -1;

    while (current_idx != -1) {
        FastOrder& order = pool.get(current_idx);
        if (order.id == target_id) {
            if (prev_idx == -1)
                level.head_order_idx = order.next_order_index;
            else
                pool.get(prev_idx).next_order_index = order.next_order_index;

            if (current_idx == level.tail_order_idx)
                level.tail_order_idx = prev_idx;

            pool.free(current_idx);
            return;
        }
        prev_idx    = current_idx;
        current_idx = order.next_order_index;
    }
}

// ── match ─────────────────────────────────────────────────────────────────────
void match(FastOrder& incoming, OrderPool& pool, int64_t& ltp) {

    // ── CANCEL ────────────────────────────────────────────────────────────────
    if (incoming.side == CANCEL) {
        int64_t price = safe_price(incoming.price);
        cancel_order(bids_book, incoming.id, price, pool);
        cancel_order(asks_book, incoming.id, price, pool);
        return;
    }

    const int64_t clamped_price = safe_price(incoming.price);

    // ── BUY ───────────────────────────────────────────────────────────────────
    if (incoming.side == BUY) {

        while (incoming.volume > 0 && best_ask <= clamped_price) {
            PriceLevel& level   = asks_book[best_ask];
            int32_t current_idx = level.head_order_idx;

            while (current_idx != -1 && incoming.volume > 0) {
                FastOrder& resting = pool.get(current_idx);
                uint32_t fill_qty  = std::min(incoming.volume, resting.volume);

                incoming.volume -= fill_qty;
                resting.volume  -= fill_qty;
                ltp = resting.price;

                if (resting.volume == 0) {
                    int32_t next_idx     = resting.next_order_index;
                    pool.free(current_idx);
                    current_idx          = next_idx;
                    level.head_order_idx = current_idx;
                }
            }

            if (level.head_order_idx == -1) {
                level.tail_order_idx = -1;
                best_ask++;
                while (best_ask < BOOK_SIZE && asks_book[best_ask].head_order_idx == -1)
                    best_ask++;
            }
        }

        if (incoming.volume > 0) {
            int32_t new_idx = pool.allocate();
            if (new_idx == -1) return;

            FastOrder& slot       = pool.get(new_idx);
            slot                  = incoming;
            slot.price            = (uint64_t)clamped_price;
            slot.next_order_index = -1;

            add_to_book(bids_book, new_idx, clamped_price, pool);
            if (clamped_price > best_bid) best_bid = clamped_price;
        }

    }
    // ── SELL ──────────────────────────────────────────────────────────────────
    else {

        while (incoming.volume > 0 && best_bid >= clamped_price && best_bid >= 0) {
            PriceLevel& level   = bids_book[best_bid];
            int32_t current_idx = level.head_order_idx;

            while (current_idx != -1 && incoming.volume > 0) {
                FastOrder& resting = pool.get(current_idx);
                uint32_t fill_qty  = std::min(incoming.volume, resting.volume);

                incoming.volume -= fill_qty;
                resting.volume  -= fill_qty;
                ltp = resting.price;

                if (resting.volume == 0) {
                    int32_t next_idx     = resting.next_order_index;
                    pool.free(current_idx);
                    current_idx          = next_idx;
                    level.head_order_idx = current_idx;
                }
            }

            if (level.head_order_idx == -1) {
                level.tail_order_idx = -1;
                best_bid--;
                while (best_bid >= 0 && bids_book[best_bid].head_order_idx == -1)
                    best_bid--;
            }
        }

        if (incoming.volume > 0) {
            int32_t new_idx = pool.allocate();
            if (new_idx == -1) return;

            FastOrder& slot       = pool.get(new_idx);
            slot                  = incoming;
            slot.price            = (uint64_t)clamped_price;
            slot.next_order_index = -1;

            add_to_book(asks_book, new_idx, clamped_price, pool);
            if (clamped_price < best_ask) best_ask = clamped_price;
        }
    }
}
