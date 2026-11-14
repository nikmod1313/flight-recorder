// Finding holes: stretches where the recorder was frozen, as when the Mac sleeps.
//
// A sleeping Mac doesn't end a session. The recorder simply stops, and when the Mac
// wakes it carries on in the same log. Replay still matches, because the log holds
// exactly what the strategy saw, but the market moved on without us in between.
//
// Every trade carries two times: when it reached us (the steady clock) and when it
// happened (Binance's exchange_ns). Between two trades in a row, both normally move
// forward by about the same amount; they differ only by how much later one trade
// arrived than the other, which is well under a second. A hole shows as:
//   - the two clocks disagreeing by more than kHoleSkew (one clock kept running while
//     the other stood still, e.g. a steady clock that stops during sleep), or
//   - both moving by more than kHoleQuiet (a long stretch with no trades at all,
//     which a busy pair like BTCUSDT doesn't have while the feed is alive).
#pragma once
#include <cstdint>

namespace fr_holes {

constexpr int64_t kHoleSkew = 5'000'000'000;    // 5 s
constexpr int64_t kHoleQuiet = 30'000'000'000;  // 30 s

struct Hole {
    int64_t arrival_jump;   // how far our clock moved across it, ns
    int64_t exchange_jump;  // how far Binance's clock moved across it, ns
};

class HoleFinder {
public:
    // Call for every trade with a known exchange time. Returns true when a hole
    // ends just before this trade; `last()` then says how big it was.
    bool on_trade(int64_t arrival_ns, int64_t exchange_ns) {
        bool hole = false;
        if (seen_) {
            last_ = {arrival_ns - arrival_, exchange_ns - exchange_};
            const int64_t skew = last_.exchange_jump - last_.arrival_jump;
            hole = skew > kHoleSkew || skew < -kHoleSkew ||
                   (last_.arrival_jump > kHoleQuiet && last_.exchange_jump > kHoleQuiet);
        }
        seen_ = true;
        arrival_ = arrival_ns;
        exchange_ = exchange_ns;
        return hole;
    }
    Hole last() const { return last_; }

private:
    bool seen_ = false;
    int64_t arrival_ = 0;
    int64_t exchange_ = 0;
    Hole last_{0, 0};
};

}  // namespace fr_holes
