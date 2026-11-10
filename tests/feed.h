// Synthetic market feed. Every number it produces is made up.
#pragma once

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "fr/flight_recorder.h"

namespace fr_test {

// Reads lines of "price qty side bid ask [bid_qty ask_qty]" from stdin. A healthy feed ends with a line saying
// "end". If input stops without it, the feed died, so the run must not look
// finished. A bad line stops the run too: a guess at what it meant would be
// recorded as truth.
class LineFeed final : public fr::TickSource {
public:
    bool next(fr::Tick& out) override {
        char line[128];
        if (!std::fgets(line, sizeof line, stdin)) {
            throw std::runtime_error(std::ferror(stdin) ? "cannot read feed" : "feed stopped without its end line");
        }
        if (std::strcmp(line, "end\n") == 0) return false;
        long long price = 0, qty = 0, side = 0, bid = 0, ask = 0, bid_qty = 0, ask_qty = 0;
        int used5 = 0, used7 = 0;
        const int fields = std::sscanf(line, "%lld %lld %lld %lld %lld %n%lld %lld %n", &price, &qty, &side, &bid, &ask,
                                       &used5, &bid_qty, &ask_qty, &used7);
        const bool five = fields == 5 && line[used5] == '\0';  // queue sizes left out: they read as 0
        const bool seven = fields == 7 && line[used7] == '\0';
        if (!five && !seven) throw std::runtime_error("bad feed line: " + std::string(line));
        out = {seq_++, price, qty, side, bid, ask, bid_qty, ask_qty};
        return true;
    }

private:
    uint64_t seq_ = 0;
};

class SyntheticFeed final : public fr::TickSource {
    static constexpr int64_t kCent = 1'000'000;

public:
    explicit SyntheticFeed(uint64_t events) : events_(events) {}

    bool next(fr::Tick& out) override {
        if (seq_ == events_) return false;
        x_ ^= x_ << 13;
        x_ ^= x_ >> 7;
        x_ ^= x_ << 17;
        price_ += (static_cast<int64_t>(x_ % 5) - 2) * kCent;
        const int64_t side = static_cast<int64_t>((x_ >> 8) % 3) - 1;
        const int64_t trade_price = side == 0 ? 0 : price_ + side * kCent;
        const int64_t qty = side == 0 ? 0 : static_cast<int64_t>(x_ >> 40);
        const int64_t bid_qty = static_cast<int64_t>((x_ >> 20) % 100 + 1) * kCent;  // made-up queue sizes
        const int64_t ask_qty = static_cast<int64_t>((x_ >> 28) % 100 + 1) * kCent;
        out = {seq_++, trade_price, qty, side, price_ - kCent, price_ + kCent, bid_qty, ask_qty};
        return true;
    }

private:
    uint64_t events_;
    uint64_t seq_ = 0;
    uint64_t x_ = 0x9E3779B97F4A7C15ull;
    int64_t price_ = 7'800'000'000'000;
};

}  // namespace fr_test
