# Order Matching Engine

A price-time priority limit order book and matching engine in C++20, built for deterministic low-latency execution rather than feature breadth. Includes a multithreaded SPSC producer/consumer simulation harness and a `ctypes`-compatible FFI layer for driving the engine from Python (e.g. for RL agent / market-simulation workloads).

```
Throughput : ~49.7 MOPS (WSL2/Linux) · ~32.4 MOPS (native Windows) — 1M mixed-flow events, single thread
Latency    : O(1) submit, cancel, and match-at-price-level
```

---

## Why this exists

Most "toy" matching engines reach for `std::map<price, std::deque<Order>>` — a red-black tree of FIFO queues. It's correct, but every insert/cancel/best-price lookup costs `O(log n)` tree traversal plus pointer-chasing through tree nodes, and the allocator is in the hot path for every order.

This engine instead treats the order book as what it actually is in most venues: a **bounded, discretized price space**. Tick sizes are fixed, so price levels can be addressed directly instead of searched for.

## Architecture

```
                 ┌─────────────────────┐
   producer ───► │   SPSC Ring Buffer    │ ───►  consumer
  (order gen)    │  (lock-free, 1P/1C)   │     (match engine)
                 └─────────────────────┘
                                                       │
                                                       ▼
                              ┌────────────────────────────────────┐
                              │         OrderPool (arena)            │
                              │  fixed array of FastOrder + free-list │
                              └────────────────────────────────────┘
                                                       │
                              ┌────────────────────────┴────────────────────────┐
                              ▼                                                   ▼
                    bids_book[price]                                   asks_book[price]
                 (flat array, index = tick price)                (flat array, index = tick price)
                              │                                                   │
                  intrusive linked list of                          intrusive linked list of
                  pool indices, FIFO per level                      pool indices, FIFO per level
```

**Order book.** `bids_book` and `asks_book` are flat `std::vector<PriceLevel>` of fixed size (1,000,000 ticks), indexed *directly* by integer tick price — `book[price]` is the lookup, full stop. `best_bid` / `best_ask` are scalar cursors that walk forward/backward as levels empty, so finding the top of book is a pointer read, not a tree-min/max.

**Orders within a level.** Orders resting at the same price are chained as a singly-linked list using a `next_order_index` field that lives *inside* the order struct itself (`orders.h`) — there's no separate linked-list node. The list is intrusive: traversal, insertion at tail, and removal from anywhere in the chain are all pointer-free array-index walks.

**Memory.** `OrderPool` (`memory_pool.h`) pre-allocates a fixed-capacity `std::vector<FastOrder>` once at startup and manages it as an arena with an embedded free list — the same `next_order_index` field doubles as the pool's free-list pointer when an order isn't resting in the book. `allocate()` and `free()` are both O(1) index operations with zero calls to `malloc`/`new` after construction.

**Concurrency.** `main.cpp` runs the order generator and the matching engine on separate threads, connected by a lock-free single-producer/single-consumer ring buffer (`spsc_queue.h`). Head and tail indices are `alignas(64)` to keep them on separate cache lines, preventing false sharing between the two threads; the queue uses relaxed loads on the owning side and acquire/release across the handoff — no mutex, no CAS loop.

**Python interop.** `ffi_bridge.cpp` exposes the engine through a flat `extern "C"` API with ABI-stable structs (`BookSnapshot`, `FillReport`, `SubmitResult`) consumable via `ctypes` with no Python C-API or `pybind11` dependency. `engine_step_batch()` accepts parallel arrays and processes a whole batch of background order flow in a single call, so an agent loop (e.g. RL-driven order submission) only crosses the Python↔C++ boundary once per batch rather than once per order.

**Synthetic order flow.** Both benchmark harnesses (`main.cpp`, `only_matching.cpp`) generate flow with a regime-switching model (neutral / uptrend / downtrend, resampled every 10k ticks) that mixes cancels (~40%), market orders (~20%, sweep ±10 ticks from anchor LTP), and limit orders (~40%, distance from anchor drawn from an `Exp(λ=0.5)` distribution — mean 2 ticks with a long tail to 10–20+ ticks) — closer to real order flow shape than uniform random placement.

## Key design decisions

| Decision | Alternative considered | Why this won |
|---|---|---|
| **Flat array indexed by tick price**, fixed at 1,000,000 entries | `std::map<price, level>` (tree) | O(1) access vs. O(log n); the cost is paid in address space, not time — if price starts at ₹100 (tick 10000), the lower ~10,000 entries sit unused. Direct tradeoff of memory for guaranteed-constant-time lookups, and at this scale the wasted space (a few MB of mostly-empty `PriceLevel` structs) is irrelevant next to the latency win. |
| **Intrusive pool-embedded linked list** for orders at a price level | Separate linked-list nodes / `std::deque` per level | No secondary allocation per order — the "node" and the order *are* the same struct, so there's no pointer-chasing into a different memory region during a busy book. |
| **Arena allocator with embedded free list** (`OrderPool`) | `new`/`delete` per order, or a generic slab allocator | Deterministic O(1) allocate/free with no allocator metadata overhead and no calls into the heap on the matching hot path — the free list reuses the same field used for in-book chaining. |
| **Lock-free SPSC ring buffer** for the producer/consumer split | `std::mutex` + `std::queue`, or `std::condition_variable` | No kernel involvement, no lock contention between the two threads; cache-line-padded head/tail avoid false sharing. This was new territory — first hands-on implementation of the SPSC pattern with explicit memory-ordering control (`relaxed`/`acquire`/`release`) instead of defaulting to `seq_cst`. |
| **Built with explicit AVX-512 target flags** (`-mavx512f -mavx512dq -mavx512bw -march=native`) rather than generic `-O3` | Hand-written intrinsics, or no ISA targeting at all | No code path is hand-vectorized — the win comes from letting clang's autovectorizer use the widest SIMD instructions available on the host CPU for the struct copies and comparisons already in the hot path. Measured difference is large enough to matter (see Benchmarks) without taking on the complexity and portability cost of manual intrinsics. |
| **`ctypes` FFI** over `pybind11` for the Python bridge | `pybind11` | Avoids a Python-C-API/pybind11 build dependency entirely; raw `extern "C"` + fixed-layout structs keep the ABI simple and let a batch of orders cross the language boundary in one call instead of one Python-level call per order. |

## Tech stack

| Layer | Choice |
|---|---|
| Core engine | C++20 |
| Compiler / flags | `clang++ -O3 -std=c++20 -march=native` (MSYS2, Windows) |
| Concurrency | `std::thread`, `std::atomic`, custom lock-free SPSC queue |
| Python interop | `ctypes` over a compiled shared library (`.dll`/`.so`) |
| License | GPLv3 |

## Repository layout

```
orders.h            Core order struct (FastOrder) and Side enum
order_book.h         Book state declarations (PriceLevel, bid/ask arrays, best bid/ask)
memory_pool.h        OrderPool — arena allocator with embedded free list
spsc_queue.h          Lock-free single-producer/single-consumer ring buffer
order_matching.cpp    match() — the actual matching logic (add/cancel/fill)
main.cpp              Multithreaded producer/consumer simulation + live book display
only_matching.cpp      Single-threaded pure-engine benchmark (no display/queue overhead)
ffi_bridge.cpp         extern "C" API for Python (ctypes) — used for agent/RL simulation
gen_random_order.cpp   Standalone synthetic depth-of-book generator (utility)
```

## Building

```bash
# Windows / MSYS2, clang++
clang++ -O3 -std=c++20 -march=native -shared -fPIC \
    -o market_engine.dll \
    ffi_bridge.cpp order_matching.cpp \
    -I. -DBUILDING_DLL

# Standalone benchmark (single-threaded, pure engine)
clang++ -O3 -std=c++20 -mavx512f -mavx512dq -mavx512bw -march=native \
    -o bench only_matching.cpp order_matching.cpp

# Multithreaded simulation with live book display
clang++ -O3 -std=c++20 -mavx512f -mavx512dq -mavx512bw -march=native \
    -o sim main.cpp order_matching.cpp -lpthread
```

## Benchmarks

`only_matching.cpp` reports throughput in million operations per second (MOPS) over 1,000,000 synthetically generated, mixed-flow orders (~40% cancel, ~20% market, ~40% limit), single-threaded, with pre-generation excluded from the timed region.

Measured across 12 consecutive runs per platform, same source, same compiler flags:

| Platform | Mean throughput | Range |
|---|---|---|
| WSL2 (Arch Linux) | **49.7 MOPS** | 47.7 – 51.1 MOPS |
| Native Windows | **32.4 MOPS** | 31.5 – 33.2 MOPS |

Build: `clang++ -O3 -std=c++20 -mavx512f -mavx512dq -mavx512bw -march=native`, run on a Ryzen 7 8845HS.

The ~1.5x gap between WSL2 and native Windows on identical source and flags is consistent and reproducible — most likely attributable to Windows heap allocator overhead and real-time AV scanning versus the lighter-weight glibc allocator and process model under WSL2, rather than anything in the engine itself.

This is a throughput measurement, not a latency distribution — it doesn't capture per-order p50/p99 latency, which would need each `match()` call timed individually rather than aggregated over the batch.

## What's next

- Per-order latency percentiles (p50/p99) rather than aggregate throughput alone
- Exact VWAP fill tracking through the FFI layer (currently approximated from LTP movement post-batch)
- Price-time priority across partial fills at the same price/timestamp tie-break
- Vectorized book scanning for multi-level sweep scenarios (currently scalar)

## License

GPLv3 — see `LICENSE`.
