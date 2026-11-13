// Synthetic market feed. Every number it produces is made up.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "fr/flight_recorder.h"

namespace fr_test {

// Reads one event per line from stdin:
//   "price qty side bid ask [bid_qty ask_qty [exchange_ns]]"   a trade or a best-price update
//   "book NB NA p q p q ..."   the best NB bid and NA ask levels, best first, as (price, qty) pairs
// A healthy feed ends with a line saying "end". If input stops without it, the
// feed died, so the run must not look finished. A bad line stops the run too: a
// guess at what it meant would be recorded as truth.
class LineFeed final : public fr::TickSource {
public:
    bool next(fr::Tick& out) override {
        static char line[1 << 16];
        if (!std::fgets(line, sizeof line, stdin)) {
            throw std::runtime_error(std::ferror(stdin) ? "cannot read feed" : "feed stopped without its end line");
        }
        const size_t length = std::strlen(line);
        if (length == 0 || line[length - 1] != '\n') throw std::runtime_error("feed line cut off or too long");
        if (std::strcmp(line, "end\n") == 0) return false;
        has_book_ = false;

        std::vector<long long> v;
        const bool is_book = std::strncmp(line, "book ", 5) == 0;
        if (!numbers(line + (is_book ? 5 : 0), v)) throw std::runtime_error("bad feed line: " + std::string(line));
        if (is_book) {
            if (v.size() < 2 || v[0] < 0 || v[1] < 0 || v.size() != 2 + 2 * static_cast<size_t>(v[0] + v[1])) {
                throw std::runtime_error("bad book line: " + std::string(line));
            }
            book_.bids.clear();
            book_.asks.clear();
            for (long long i = 0; i < v[0]; ++i) book_.bids.push_back({v[2 + 2 * i], v[3 + 2 * i]});
            for (long long i = 0; i < v[1]; ++i) book_.asks.push_back({v[2 + 2 * (v[0] + i)], v[3 + 2 * (v[0] + i)]});
            has_book_ = true;
            const fr::BookLevel none{0, 0};
            const fr::BookLevel bid = book_.bids.empty() ? none : book_.bids[0];
            const fr::BookLevel ask = book_.asks.empty() ? none : book_.asks[0];
            out = {seq_++, 0, 0, 0, bid.price, ask.price, bid.qty, ask.qty, 0};
            return true;
        }
        if (v.size() != 5 && v.size() != 7 && v.size() != 8) {  // left-out fields read as 0
            throw std::runtime_error("bad feed line: " + std::string(line));
        }
        v.resize(8, 0);
        out = {seq_++, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
        return true;
    }

    const fr::Book* book() const override { return has_book_ ? &book_ : nullptr; }

private:
    // Whole numbers separated by spaces, and nothing else.
    static bool numbers(const char* text, std::vector<long long>& out) {
        while (true) {
            while (*text == ' ') ++text;
            if (*text == '\n' || *text == '\0') return true;
            char* after = nullptr;
            errno = 0;
            const long long value = std::strtoll(text, &after, 10);
            if (after == text || errno != 0 || (*after != ' ' && *after != '\n')) return false;
            out.push_back(value);
            text = after;
        }
    }

    uint64_t seq_ = 0;
    fr::Book book_;
    bool has_book_ = false;
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
        out = {seq_++, trade_price, qty, side, price_ - kCent, price_ + kCent, bid_qty, ask_qty, 0};
        return true;
    }

private:
    uint64_t events_;
    uint64_t seq_ = 0;
    uint64_t x_ = 0x9E3779B97F4A7C15ull;
    int64_t price_ = 7'800'000'000'000;
};

}  // namespace fr_test
