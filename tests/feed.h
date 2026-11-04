// Synthetic market feed. Every number it produces is made up.
#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>

#include "fr/flight_recorder.h"

namespace fr_test {

// Reads lines of "price qty" from stdin. The feed ends at end of input. A bad
// line stops the run: a guess at what it meant would be recorded as truth.
class LineFeed final : public fr::TickSource {
public:
    bool next(fr::Tick& out) override {
        char line[128];
        if (!std::fgets(line, sizeof line, stdin)) {
            if (std::ferror(stdin)) throw std::runtime_error("cannot read feed");
            return false;
        }
        long long price = 0, qty = 0;
        int used = 0;
        if (std::sscanf(line, "%lld %lld %n", &price, &qty, &used) != 2 || line[used] != '\0') {
            throw std::runtime_error("bad feed line: " + std::string(line));
        }
        out = {seq_++, price, qty};
        return true;
    }

private:
    uint64_t seq_ = 0;
};

class SyntheticFeed final : public fr::TickSource {
public:
    explicit SyntheticFeed(uint64_t events) : events_(events) {}

    bool next(fr::Tick& out) override {
        if (seq_ == events_) return false;
        x_ ^= x_ << 13;
        x_ ^= x_ >> 7;
        x_ ^= x_ << 17;
        price_ += static_cast<int64_t>(x_ % 5) - 2;
        out = {seq_++, price_, static_cast<int64_t>(x_ >> 54) + 1};
        return true;
    }

private:
    uint64_t events_;
    uint64_t seq_ = 0;
    uint64_t x_ = 0x9E3779B97F4A7C15ull;
    int64_t price_ = 1'000'000;
};

}  // namespace fr_test
