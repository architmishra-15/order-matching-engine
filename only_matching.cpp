#if defined(_WIN64)
#include <windows.h>
#endif

#include <random>
#include <chrono>
#include <vector>
#include <cstdio>

// ── Include your existing local headers ───────────────────────────────────────
#include "orders.h"
#include "memory_pool.h"
#include "order_book.h"

constexpr size_t TOTAL_TEST_ORDERS = 1000000;
enum class MarketRegime { NEUTRAL, UPTREND, DOWNTREND };

struct ActiveOrderTracker {
    uint64_t id;
    uint64_t price;
};

// Extern declaration for your matching function from order_matching.cpp
extern void match(FastOrder& incoming, OrderPool& pool, int64_t& ltp);

int main() {
#if defined(_WIN64)
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ── PHASE 1: Pre-Generation (Untimed) ─────────────────────────────────────
    printf("Pre-generating %zu orders (This does not count towards benchmark)...\n", TOTAL_TEST_ORDERS);
    
    std::vector<FastOrder> pregen_orders;
    pregen_orders.reserve(TOTAL_TEST_ORDERS);

    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
    std::uniform_int_distribution<int>     size_dist(1, 20);
    std::exponential_distribution<double>  price_dist(0.5);

    MarketRegime current_regime = MarketRegime::NEUTRAL;
    std::vector<ActiveOrderTracker> live_orders;
    live_orders.reserve(4096);

    uint64_t id_counter   = 0;
    int64_t  anchor_ltp   = 10000; // Simulated LTP for generation

    for (size_t push_counter = 0; push_counter < TOTAL_TEST_ORDERS; ++push_counter) {
        if (push_counter % 10000 == 0) {
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

        FastOrder new_order{};

        // CANCEL (~40%)
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
        // MARKET ORDER (~20%)
        else if (event_roll < 0.60 || live_orders.empty()) {
            int64_t raw = is_buy ? (anchor_ltp + 10) : (anchor_ltp - 10);
            if (raw < 1)      raw = 1;
            if (raw > 999998) raw = 999998;

            new_order.id     = ++id_counter;
            new_order.volume = size_dist(rng);
            new_order.side   = is_buy ? BUY : SELL;
            new_order.price  = (uint64_t)raw;
        }
        // LIMIT ORDER (~40%)
        else {
            int64_t tick_distance = static_cast<int64_t>(price_dist(rng) + 0.5);
            if (tick_distance < 1) tick_distance = 1;

            int64_t raw = is_buy ? (anchor_ltp - tick_distance) : (anchor_ltp + tick_distance);
            if (raw < 1)      raw = 1;
            if (raw > 999998) raw = 999998;

            new_order.id     = ++id_counter;
            new_order.volume = size_dist(rng);
            new_order.side   = is_buy ? BUY : SELL;
            new_order.price  = (uint64_t)raw;

            live_orders.push_back({ new_order.id, new_order.price });
        }
        
        pregen_orders.push_back(new_order);
    }

    printf("Generation complete. Engaging matching engine...\n\n");

    // ── PHASE 2: Core Benchmark (Timed) ───────────────────────────────────────
    OrderPool order_pool(2000000);
    int64_t live_ltp = 10000;

    auto start_time = std::chrono::high_resolution_clock::now();

    // The CPU is now doing absolutely nothing but executing your matching logic
    for (size_t i = 0; i < TOTAL_TEST_ORDERS; ++i) {
        match(pregen_orders[i], order_pool, live_ltp);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    
    // ── Results ───────────────────────────────────────────────────────────────
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    double mops    = (static_cast<double>(TOTAL_TEST_ORDERS) / elapsed) / 1e6;

    printf("--- PURE ENGINE BENCHMARK RESULTS ---\n");
    printf("Events processed : %llu\n", (unsigned long long)TOTAL_TEST_ORDERS);
    printf("Time elapsed     : %.6f s\n",    elapsed);
    printf("Throughput       : %.4f MOPS\n", mops);
    printf("Final LTP        : %.2f\n",      live_ltp / 100.0);

    return 0;
}
