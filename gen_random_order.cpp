// gen_random_order.cpp
#include <iostream>
#include <ranges>
#include <algorithm> // Required for std::ranges::sort
#include <random>
#include <vector>

constexpr int default_depth = 5;

std::vector<std::vector<int>> gen_rand_order(int depth = default_depth) {
  std::random_device rd;
  std::mt19937 gen(rd());

  std::vector<std::vector<int>> order;

  // 1. Generate base LTP (e.g., 10.00 to 1000.00)
  std::uniform_int_distribution<int> ltp_dis(1000, 100000); 
  int ltp = ltp_dis(gen);

  std::uniform_real_distribution<float> spread_dis(0.0005, 0.001);
  float spread = spread_dis(gen);

  int spread_pips = ltp * spread;
  // if (spread_pips < 1) spread_pips = 1;

  std::vector<int> bid_depth(depth);
  std::vector<int> ask_depth(depth);

  // 2. Distributions
  std::uniform_int_distribution<int> bid_dis(ltp - spread_pips, ltp - 5); // -5 to ensure a spread
  std::uniform_int_distribution<int> ask_dis(ltp + 5, ltp + spread_pips);

  for (int i = 0; i < depth; i++) {
    bid_depth[i] = bid_dis(gen);
    ask_depth[i] = ask_dis(gen);
  }

  // 3. Proper Sorting
  // Bids: Highest price first (Descending)
  std::ranges::sort(bid_depth, std::ranges::greater{}); 

  // Asks: Lowest price first (Ascending)
  std::ranges::sort(ask_depth);

  std::cout << "Spread Pips: " << spread_pips << "\n";

  std::cout << "LTP: " << float(ltp)/100 << "\n";
  std::cout << "Market Depth:\n";

  std::cout << "--- ASKS (Sellers) ---\n";
  for (const auto& price : ask_depth | std::views::reverse) // Print highest ask at top
  std::cout << "Price: " << float(price)/100 << "\n";

  std::cout << "--- BIDS (Buyers) ---\n";
  for (const auto& price : bid_depth)
  std::cout << "Price: " << float(price)/100 << "\n";

  order.push_back(bid_depth);
  order.push_back(ask_depth);
  return order;
}
