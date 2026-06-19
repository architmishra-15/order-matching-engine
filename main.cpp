// main.cpp
#if defined(_WIN64)
#include <windows.h>
#endif

#include <random>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdio>

#include "orders.h"
#include "memory_pool.h"
#include "order_book.h"
#include "spsc_queue.h"

constexpr size_t TOTAL_TEST_ORDERS = 1000000;
constexpr int    DISPLAY_DEPTH     = 5;
constexpr int    DISPLAY_LINES     = DISPLAY_DEPTH * 2 + 5;

enum class MarketRegime { NEUTRAL, UPTREND, DOWNTREND };

struct ActiveOrderTracker {
    uint64_t id;
    uint64_t price;
};

// ── Shared infrastructure ─────────────────────────────────────────────────────
SPSCQueue<FastOrder>  order_queue(65536);
OrderPool             order_pool(2000000);

std::atomic<int64_t>  current_ltp{ 10000 };
std::atomic<bool>     start_flag{ false };
std::atomic<uint64_t> events_processed{ 0 };

extern void match(FastOrder& incoming, OrderPool& pool, int64_t& ltp);

// ── Display ───────────────────────────────────────────────────────────────────

static uint64_t level_volume(int64_t price, bool is_bid) {
    if (price < 0 || price >= BOOK_SIZE) return 0;
    auto&    book  = is_bid ? bids_book : asks_book;
    uint64_t total = 0;
    int32_t  idx   = book[price].head_order_idx;
    while (idx != -1) {
        total += order_pool.get(idx).volume;
        idx    = order_pool.get(idx).next_order_index;
    }
    return total;
}

void print_book(int64_t ltp, uint64_t events, double elapsed) {
    printf("  %-12s %10s   %s\n", "Price", "Volume", "Side");
    printf("  %s\n", "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80");

    // Collect up to DISPLAY_DEPTH ask levels starting from best_ask
    int64_t ask_levels[DISPLAY_DEPTH];
    int     ask_count = 0;
    for (int64_t p = best_ask; p < BOOK_SIZE && ask_count < DISPLAY_DEPTH; p++)
        if (asks_book[p].head_order_idx != -1)
            ask_levels[ask_count++] = p;

    // Print highest ask first (furthest from mid at top)
    for (int i = ask_count - 1; i >= 0; i--) {
        uint64_t vol = level_volume(ask_levels[i], false);
        printf("  \033[31m%12.2f\033[0m %10llu   ASK\n",
               ask_levels[i] / 100.0, (unsigned long long)vol);
    }
    for (int i = ask_count; i < DISPLAY_DEPTH; i++)
        printf("  %12s %10s   ASK\n", "---", "---");

    // LTP / spread
    double spread = (best_ask < BOOK_SIZE && best_bid >= 0)
                  ? (best_ask - best_bid) / 100.0 : 0.0;
    printf("  \033[1;33m LTP %8.2f   Spread: %.2f\033[0m\n",
           ltp / 100.0, spread);

    // Collect up to DISPLAY_DEPTH bid levels starting from best_bid
    int64_t bid_levels[DISPLAY_DEPTH];
    int     bid_count = 0;
    for (int64_t p = best_bid; p >= 0 && bid_count < DISPLAY_DEPTH; p--)
        if (bids_book[p].head_order_idx != -1)
            bid_levels[bid_count++] = p;

    for (int i = 0; i < DISPLAY_DEPTH; i++) {
        if (i < bid_count) {
            uint64_t vol = level_volume(bid_levels[i], true);
            printf("  \033[32m%12.2f\033[0m %10llu   BID\n",
                   bid_levels[i] / 100.0, (unsigned long long)vol);
        } else {
            printf("  %12s %10s   BID\n", "---", "---");
        }
    }

    printf("  %s\n", "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80");
    printf("  Events: %7llu / %llu   %.3fs\n",
           (unsigned long long)events,
           (unsigned long long)TOTAL_TEST_ORDERS,
           elapsed);
}

// ── Producer ──────────────────────────────────────────────────────────────────
void generator_thread() {
    while (!start_flag.load(std::memory_order_acquire)) {}

    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
    std::uniform_int_distribution<int>     size_dist(1, 20);

    // lambda=0.5 gives a flatter exponential — orders spread across a wider
    // range from mid rather than 86% clustering at tick 0 or 1.
    // Mean distance = 1/lambda = 2.0, so typical limit order is ~2 ticks out,
    // but the long tail reaches 10-20+ ticks naturally.
    std::exponential_distribution<double>  price_dist(0.5);

    MarketRegime current_regime = MarketRegime::NEUTRAL;
    std::vector<ActiveOrderTracker> live_orders;
    live_orders.reserve(4096);

    uint64_t id_counter   = 0;
    uint64_t push_counter = 0;
    uint64_t tick_counter = 0;

    while (push_counter < TOTAL_TEST_ORDERS) {

        if (tick_counter++ % 10000 == 0) {
            double r = prob_dist(rng);
            if      (r < 0.750) current_regime = MarketRegime::NEUTRAL;
            else if (r < 0.875) current_regime = MarketRegime::UPTREND;
            else                current_regime = MarketRegime::DOWNTREND;
        }

        double buy_probability = 0.50;
        if (current_regime == MarketRegime::UPTREND)   buy_probability = 0.85;
        if (current_regime == MarketRegime::DOWNTREND) buy_probability = 0.15;

        const bool    is_buy     = prob_dist(rng) < buy_probability;
        const double  event_roll = prob_dist(rng);
        const int64_t anchor     = current_ltp.load(std::memory_order_relaxed);

        FastOrder new_order{};

        // ── CANCEL (~40%) ─────────────────────────────────────────────────────
        if (event_roll < 0.40 && !live_orders.empty()) {
            std::uniform_int_distribution<size_t> idx_dist(0, live_orders.size() - 1);
            size_t rand_idx = idx_dist(rng);

            new_order.id     = live_orders[rand_idx].id;
            new_order.price  = live_orders[rand_idx].price;
            new_order.side   = CANCEL;
            new_order.volume = 0;

            live_orders[rand_idx] = live_orders.back();
            live_orders.pop_back();
        }
        // ── MARKET ORDER (~20%) ───────────────────────────────────────────────
        // ±10 ticks (±₹0.10) sweeps a few levels of real resting liquidity
        // without slamming the price artificially far from mid.
        else if (event_roll < 0.60 || live_orders.empty()) {
            int64_t raw = is_buy ? (anchor + 10) : (anchor - 10);
            if (raw < 1)      raw = 1;
            if (raw > 999998) raw = 999998;

            new_order.id     = ++id_counter;
            new_order.volume = size_dist(rng);
            new_order.side   = is_buy ? BUY : SELL;
            new_order.price  = (uint64_t)raw;
        }
        // ── LIMIT ORDER (~40%) ────────────────────────────────────────────────
        // Exp(0.5) has mean=2.0, so raw samples are typically 0.3–5.0.
        // Rounding to nearest int gives tick distances of 1, 2, 3 ... with
        // natural exponential decay — no scaling needed. Minimum 1 tick from mid.
        else {
            int64_t tick_distance = static_cast<int64_t>(price_dist(rng) + 0.5);
            if (tick_distance < 1) tick_distance = 1;

            int64_t raw = is_buy ? (anchor - tick_distance) : (anchor + tick_distance);
            if (raw < 1)      raw = 1;
            if (raw > 999998) raw = 999998;

            new_order.id     = ++id_counter;
            new_order.volume = size_dist(rng);
            new_order.side   = is_buy ? BUY : SELL;
            new_order.price  = (uint64_t)raw;

            live_orders.push_back({ new_order.id, new_order.price });
        }

        while (!order_queue.push(new_order)) {}
        push_counter++;
    }
}

// ── Consumer ──────────────────────────────────────────────────────────────────
void engine_thread() {
    while (!start_flag.load(std::memory_order_acquire)) {}

    FastOrder incoming;
    uint64_t  local_count = 0;
    int64_t   local_ltp   = current_ltp.load(std::memory_order_relaxed);

    while (local_count < TOTAL_TEST_ORDERS) {
        if (order_queue.pop(incoming)) {
            match(incoming, order_pool, local_ltp);
            current_ltp.store(local_ltp, std::memory_order_relaxed);
            local_count++;
            if (local_count % 1000 == 0)
                events_processed.store(local_count, std::memory_order_relaxed);
        }
    }
    events_processed.store(local_count, std::memory_order_relaxed);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
#if defined(_WIN64)
    SetConsoleOutputCP(CP_UTF8);
    // Also enable ANSI escape processing on Windows Terminal
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

    for (int i = 0; i < DISPLAY_LINES; i++) printf("\n");

    std::thread producer(generator_thread);
    std::thread consumer(engine_thread);

    auto start_time = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);

    // Display loop — 20 Hz
    while (events_processed.load(std::memory_order_relaxed) < TOTAL_TEST_ORDERS) {
        auto   now     = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();

        printf("\033[%dA", DISPLAY_LINES);
        print_book(
            current_ltp.load(std::memory_order_relaxed),
            events_processed.load(std::memory_order_relaxed),
            elapsed
        );
        fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    producer.join();
    consumer.join();

    auto   end_time = std::chrono::high_resolution_clock::now();
    double elapsed  = std::chrono::duration<double>(end_time - start_time).count();
    double mops     = (static_cast<double>(TOTAL_TEST_ORDERS) / elapsed) / 1e6;

    printf("\033[%dA", DISPLAY_LINES);
    print_book(current_ltp.load(), TOTAL_TEST_ORDERS, elapsed);
    fflush(stdout);

    printf("\n--- BENCHMARK RESULTS ---\n");
    printf("Events processed : %llu\n",      (unsigned long long)TOTAL_TEST_ORDERS);
    printf("Time elapsed     : %.4f s\n",    elapsed);
    printf("Throughput       : %.4f MOPS\n", mops);
    printf("Final LTP        : %.2f\n",      current_ltp.load() / 100.0);

    return 0;
}
