// ffi_bridge.cpp
// Exposes the C++ matching engine to Python via ctypes.
// Compile on Windows:
//   clang++ -O3 -std=c++20 -march=native -shared -fPIC
//           -o market_engine.dll
//           ffi_bridge.cpp order_matching.cpp
//           -I. -DBUILDING_DLL
//
// All prices are integer ticks (×100), e.g. ₹100.00 = 10000.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include "orders.h"
#include "memory_pool.h"
#include "order_book.h"

// ── Engine state ──────────────────────────────────────────────────────────────
static OrderPool engine_pool(2000000);
static int64_t   engine_ltp = 10000; // ₹100.00 starting price

// ── Book snapshot struct (passed back to Python) ──────────────────────────────
// Python reads this via ctypes Structure — layout must stay stable.
struct BookLevel {
    int64_t  price;   // tick price (divide by 100 for rupees)
    uint64_t volume;  // total resting volume at this level
};

struct BookSnapshot {
    BookLevel bids[5]; // best bid first (highest price)
    BookLevel asks[5]; // best ask first (lowest price)
    int64_t   ltp;
    int64_t   best_bid_price;
    int64_t   best_ask_price;
    int64_t   spread;  // best_ask - best_bid in ticks
};

// ── Fill report (one per matched trade, returned to Python) ───────────────────
struct FillReport {
    uint64_t order_id;
    int64_t  fill_price;
    uint32_t fill_volume;
    int32_t  side; // 0=BUY, 1=SELL
};

// ── Agent order submission result ─────────────────────────────────────────────
struct SubmitResult {
    uint64_t order_id;        // assigned ID (0 = rejected)
    uint32_t filled_volume;   // how much filled immediately
    int64_t  avg_fill_price;  // volume-weighted avg fill price (ticks), 0 if no fill
    int32_t  resting_volume;  // volume left resting in book (-1 if fully filled)
};

// ── Helper: sum volume at a price level ───────────────────────────────────────
static uint64_t level_vol(int64_t price, bool is_bid) {
    if (price < 0 || price >= BOOK_SIZE) return 0;
    auto&    book = is_bid ? bids_book : asks_book;
    uint64_t vol  = 0;
    int32_t  idx  = book[price].head_order_idx;
    while (idx != -1) {
        vol += engine_pool.get(idx).volume;
        idx  = engine_pool.get(idx).next_order_index;
    }
    return vol;
}

extern void match(FastOrder& incoming, OrderPool& pool, int64_t& ltp);

// ── Monotonic order ID counter ────────────────────────────────────────────────
static uint64_t next_order_id = 1;

// ─────────────────────────────────────────────────────────────────────────────
// PUBLIC API (extern "C" so ctypes can find symbols by name)
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

// ── 1. Initialise / reset engine state ───────────────────────────────────────
// Call once at startup. safe to call again for a soft reset — clears the book.
void engine_init(int64_t starting_ltp_ticks) {
    engine_ltp    = starting_ltp_ticks;
    next_order_id = 1;

    // Reset book vectors and best bid/ask sentinels
    std::fill(bids_book.begin(), bids_book.end(), PriceLevel{-1, -1});
    std::fill(asks_book.begin(), asks_book.end(), PriceLevel{-1, -1});
    best_bid = -1;
    best_ask = BOOK_SIZE;

    // Rebuild the pool free list (re-initialise without realloc)
    // We do this by constructing a fresh pool in-place via placement new.
    engine_pool.~OrderPool();
    new (&engine_pool) OrderPool(2000000);
}

// ── 2. Submit a single agent order ───────────────────────────────────────────
// side: 0 = BUY, 1 = SELL, 2 = CANCEL
// For CANCEL, pass the original order_id in cancel_id and price of that order.
// Returns a SubmitResult describing what happened.
SubmitResult engine_submit(
    int32_t  side,
    int64_t  price_ticks,
    uint32_t volume,
    uint64_t cancel_id   // only used when side == 2 (CANCEL)
) {
    SubmitResult result{};

    if (side == 2) {
        // CANCEL path
        FastOrder cancel_order{};
        cancel_order.id     = cancel_id;
        cancel_order.price  = (uint64_t)std::clamp(price_ticks,
                                                    PRICE_MIN, PRICE_MAX);
        cancel_order.side   = CANCEL;
        cancel_order.volume = 0;
        match(cancel_order, engine_pool, engine_ltp);
        result.order_id = cancel_id;
        result.resting_volume = -1;
        return result;
    }

    FastOrder order{};
    order.id     = next_order_id++;
    order.price  = (uint64_t)std::clamp(price_ticks, PRICE_MIN, PRICE_MAX);
    order.volume = volume;
    order.side   = (side == 0) ? BUY : SELL;

    uint32_t vol_before = order.volume;
    int64_t  ltp_before = engine_ltp;

    match(order, engine_pool, engine_ltp);

    uint32_t filled = vol_before - order.volume;

    result.order_id      = order.id;
    result.filled_volume = filled;
    result.resting_volume = (int32_t)order.volume; // 0 if fully filled

    // Approximate avg fill price from LTP movement
    // (exact VWAP would need fill-by-fill tracking — acceptable approximation)
    result.avg_fill_price = (filled > 0) ? engine_ltp : 0;

    return result;
}

// ── 3. Process a batch of ZI background orders ────────────────────────────────
// Python calls this to advance the simulation N steps between agent decisions.
// ids, sides, prices, volumes are parallel arrays of length batch_size.
// sides: 0=BUY, 1=SELL, 2=CANCEL
// Returns new LTP after processing the batch.
int64_t engine_step_batch(
    uint64_t* ids,
    int32_t*  sides,
    int64_t*  prices,
    uint32_t* volumes,
    size_t    batch_size
) {
    for (size_t i = 0; i < batch_size; ++i) {
        FastOrder o{};
        o.id     = ids[i];
        o.price  = (uint64_t)std::clamp(prices[i], PRICE_MIN, PRICE_MAX);
        o.volume = volumes[i];
        o.side   = (sides[i] == 0) ? BUY
                 : (sides[i] == 1) ? SELL
                 :                   CANCEL;
        match(o, engine_pool, engine_ltp);
    }
    return engine_ltp;
}

// ── 4. Get current book snapshot ─────────────────────────────────────────────
// Fills the provided BookSnapshot pointer. Python passes a ctypes struct by ref.
void engine_get_snapshot(BookSnapshot* out) {
    out->ltp            = engine_ltp;
    out->best_bid_price = best_bid;
    out->best_ask_price = (best_ask < BOOK_SIZE) ? best_ask : -1;
    out->spread         = (best_ask < BOOK_SIZE && best_bid >= 0)
                        ? (best_ask - best_bid) : -1;

    // Fill bid levels (best = highest price first)
    int64_t p       = best_bid;
    int     filled  = 0;
    while (p >= 0 && filled < 5) {
        if (bids_book[p].head_order_idx != -1) {
            out->bids[filled].price  = p;
            out->bids[filled].volume = level_vol(p, true);
            filled++;
        }
        p--;
    }
    for (int i = filled; i < 5; i++)
        out->bids[i] = {-1, 0};

    // Fill ask levels (best = lowest price first)
    p      = (best_ask < BOOK_SIZE) ? best_ask : BOOK_SIZE;
    filled = 0;
    while (p < BOOK_SIZE && filled < 5) {
        if (asks_book[p].head_order_idx != -1) {
            out->asks[filled].price  = p;
            out->asks[filled].volume = level_vol(p, false);
            filled++;
        }
        p++;
    }
    for (int i = filled; i < 5; i++)
        out->asks[i] = {-1, 0};
}

// ── 5. Get current LTP ────────────────────────────────────────────────────────
int64_t engine_get_ltp() {
    return engine_ltp;
}

// ── 6. Get spread in ticks ────────────────────────────────────────────────────
int64_t engine_get_spread() {
    if (best_ask >= BOOK_SIZE || best_bid < 0) return -1;
    return best_ask - best_bid;
}

} // extern "C"
