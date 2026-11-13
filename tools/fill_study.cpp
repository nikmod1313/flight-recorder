// Fill study: if an order had joined the back of the queue at the best price,
// when would it have been filled?   fill_study [--flush] <out.csv> <log> [<log> ...]
//
// The book shows how much waits at each price, not who is where, so a fill can't
// be observed directly. Every 100 ms book snapshot starts two "probes", one at the
// best bid and one at the best ask, each joining behind the whole queue there.
// Each probe is then followed for 30 seconds, and four rules say whether it filled:
//   touched: a trade happened at its price (the optimistic upper bound)
//   certain: trades at its price used up the whole queue ahead, or the price traded
//            through it (a lower bound that the data proves)
//   strict:  the price traded through it (Ravi's current rule)
//   queue:   the queue ahead shrinks with trades at its price and with cancellations,
//            estimated from how the level's size changes, spread evenly over the queue
// It writes one row per probe: what was known when it joined, and the rules'
// answers at 1, 5 and 30 seconds. Logs must replay exactly with Ravi.
// --flush also writes probes still running when a day ends, with the answers so far
// (for tests on short, hand-made logs).
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "real_ravi.h"

namespace {

constexpr uint64_t kSecond = 1'000'000'000;
constexpr uint64_t kHorizons[] = {1 * kSecond, 5 * kSecond, 30 * kSecond};
constexpr int kRules = 4;  // touched, certain, strict, queue
const char* const kRuleNames[kRules] = {"touched", "certain", "strict", "queue"};

double btc(int64_t raw) { return raw / 1e8; }      // feed sizes are scaled by 100,000,000
double cents(int64_t raw) { return raw / 1e6; }    // and so are prices: 1 cent is 1,000,000

struct Probe {
    int side;  // +1 waits to buy at the bid, -1 waits to sell at the ask
    int day;
    uint64_t t;
    int64_t price;
    double ahead_certain;  // queue ahead, used up by trades only
    double ahead_queue;    // queue ahead, used up by trades and estimated cancellations
    double level_size;     // the level's size at the last snapshot, for the cancellation estimate
    double traded_since_snapshot = 0;
    uint64_t filled_at[kRules] = {0, 0, 0, 0};  // 0 means not yet
    std::string features;
};

struct Study final : fr::Strategy {
    fr_ravi::RealRavi ravi;  // the strategy the log was recorded with, so replay is checked
    FILE* out = nullptr;
    int day = 0;
    std::deque<Probe> probes;
    const fr::Book* pending_book = nullptr;
    fr::Book book;
    // Recent trades, for the features: time, side, size.
    struct Trade { uint64_t t; int side; double qty; };
    std::deque<Trade> recent;
    std::deque<std::pair<uint64_t, double>> mids;  // time, mid in cents
    uint64_t rows = 0;

    void on_book(const fr::Book& b) override {
        book = b;
        pending_book = &book;
        ravi.on_book(b);
    }

    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        struct Capture final : fr::Gate {
            fr::Gate& inner;
            uint64_t t = 0;
            explicit Capture(fr::Gate& g) : inner(g) {}
            uint64_t now_ns() override { return t = inner.now_ns(); }
            uint64_t random_u64() override { return inner.random_u64(); }
        } capture(gate);
        const int64_t out_value = ravi.on_tick(tick, capture);
        const uint64_t t = capture.t;

        if (tick.side != 0) on_trade(tick, t);
        if (pending_book) {
            on_snapshot(t);
            pending_book = nullptr;
        }
        retire(t);
        return out_value;
    }

    uint64_t state_hash() const override { return ravi.state_hash(); }

    void on_trade(const fr::Tick& tick, uint64_t t) {
        const double qty = btc(tick.qty);
        recent.push_back({t, static_cast<int>(tick.side), qty});
        while (!recent.empty() && recent.front().t + 5 * kSecond < t) recent.pop_front();
        for (Probe& p : probes) {
            if (t <= p.t) continue;
            // A seller hits bids, a buyer lifts asks: only the opposite side can fill a probe.
            if (tick.side != -p.side) continue;
            const bool at_price = tick.price == p.price;
            const bool through = p.side > 0 ? tick.price < p.price : tick.price > p.price;
            if (!at_price && !through) continue;
            mark(p, 0, t);  // touched
            if (through) {
                mark(p, 1, t);
                mark(p, 2, t);
                mark(p, 3, t);
                continue;
            }
            p.ahead_certain -= qty;
            p.ahead_queue -= qty;
            p.traded_since_snapshot += qty;
            if (p.ahead_certain < 0) mark(p, 1, t);  // < 0: the whole queue ahead traded, and some of us
            if (p.ahead_queue < 0) mark(p, 3, t);
        }
    }

    void on_snapshot(uint64_t t) {
        // Cancellations: the level shrank by more than the trades explain. Assume they
        // came evenly from the whole queue, so the share ahead of the probe is ahead/size.
        for (Probe& p : probes) {
            if (t <= p.t || p.filled_at[3]) continue;
            const auto& levels = p.side > 0 ? book.bids : book.asks;
            double size = 0;
            bool present = false;
            for (const fr::BookLevel& l : levels) {
                if (l.price == p.price) {
                    size = btc(l.qty);
                    present = true;
                    break;
                }
            }
            if (!present) continue;  // the level moved out of view; leave the estimate as it was
            const double cancelled = p.level_size - p.traded_since_snapshot - size;
            if (cancelled > 0 && p.level_size > 0 && p.ahead_queue > 0) {
                p.ahead_queue -= cancelled * (p.ahead_queue / p.level_size);
                if (p.ahead_queue < 0) p.ahead_queue = 0;  // cancellations alone can't fill an order
            }
            p.level_size = size;
            p.traded_since_snapshot = 0;
        }

        if (book.bids.empty() || book.asks.empty()) return;
        const double mid = (cents(book.bids[0].price) + cents(book.asks[0].price)) / 2;
        mids.push_back({t, mid});
        while (!mids.empty() && mids.front().first + 5 * kSecond < t) mids.pop_front();
        const std::string shared = features(t, mid);
        for (int side : {+1, -1}) {
            const fr::BookLevel& best = side > 0 ? book.bids[0] : book.asks[0];
            const fr::BookLevel& other = side > 0 ? book.asks[0] : book.bids[0];
            Probe p{side, day, t, best.price, btc(best.qty), btc(best.qty), btc(best.qty), 0, {0, 0, 0, 0}, {}};
            char buffer[160];
            std::snprintf(buffer, sizeof buffer, "%d,%.8f,%.8f,%.4f,", side, btc(best.qty), btc(other.qty),
                          (btc(best.qty) - btc(other.qty)) / (btc(best.qty) + btc(other.qty)));
            p.features = buffer + shared;
            probes.push_back(std::move(p));
        }
    }

    // Features shared by both probes: what was known at time t, and nothing later.
    std::string features(uint64_t t, double mid) const {
        double depth[2] = {0, 0};  // the best 5 levels' total size, bids then asks
        for (size_t i = 0; i < 5 && i < book.bids.size(); ++i) depth[0] += btc(book.bids[i].qty);
        for (size_t i = 0; i < 5 && i < book.asks.size(); ++i) depth[1] += btc(book.asks[i].qty);
        double buy1 = 0, sell1 = 0, buy5 = 0, sell5 = 0;
        int trades1 = 0;
        for (const Trade& tr : recent) {
            const bool last_second = tr.t + kSecond >= t;
            (tr.side > 0 ? buy5 : sell5) += tr.qty;
            if (last_second) {
                (tr.side > 0 ? buy1 : sell1) += tr.qty;
                ++trades1;
            }
        }
        double move5 = mids.empty() ? 0 : mid - mids.front().second;
        double wiggle = 0;  // how much the mid moved, step by step, over the last 5 s
        for (size_t i = 1; i < mids.size(); ++i) wiggle += std::fabs(mids[i].second - mids[i - 1].second);
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "%.2f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%d,%.2f,%.2f",
                      cents(book.asks[0].price) - cents(book.bids[0].price), depth[0], depth[1], buy1, sell1, buy5,
                      sell5, trades1, move5, wiggle);
        return buffer;
    }

    void mark(Probe& p, int rule, uint64_t t) {
        if (!p.filled_at[rule]) p.filled_at[rule] = t;
    }

    // A probe is written once its 30 seconds are over. Probes still running when the day ends are dropped.
    void retire(uint64_t t, bool everything = false) {
        while (!probes.empty() && (everything || probes.front().t + kHorizons[2] <= t)) {
            const Probe& p = probes.front();
            std::fprintf(out, "%d,%llu,%s", p.day, static_cast<unsigned long long>(p.t), p.features.c_str());
            for (int r = 0; r < kRules; ++r) {
                for (uint64_t h : kHorizons) {
                    std::fprintf(out, ",%d", p.filled_at[r] && p.filled_at[r] <= p.t + h ? 1 : 0);
                }
            }
            std::fprintf(out, "\n");
            ++rows;
            probes.pop_front();
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    const bool flush = argc > 1 && std::string(argv[1]) == "--flush";
    if (flush) {
        --argc;
        ++argv;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: fill_study [--flush] <out.csv> <log> [<log> ...]\n");
        return 64;
    }
    FILE* out = std::fopen(argv[1], "w");
    if (!out) {
        std::perror(argv[1]);
        return 1;
    }
    std::fprintf(out, "day,t_ns,side,queue_btc,other_queue_btc,queue_imbalance,spread_cents,bid_depth5_btc,"
                      "ask_depth5_btc,buy_1s_btc,sell_1s_btc,buy_5s_btc,sell_5s_btc,trades_1s,mid_move_5s_cents,"
                      "mid_wiggle_5s_cents");
    for (const char* rule : kRuleNames) {
        for (const char* h : {"1s", "5s", "30s"}) std::fprintf(out, ",%s_%s", rule, h);
    }
    std::fprintf(out, "\n");

    uint64_t total = 0;
    for (int i = 2; i < argc; ++i) {
        Study study;
        study.out = out;
        study.day = i - 1;
        const fr::ReplayReport report = fr::replay_run(study, argv[i]);
        if (flush) study.retire(0, true);
        if (report.status != fr::ReplayStatus::Match && report.status != fr::ReplayStatus::TornTail) {
            std::fprintf(stderr, "%s: replay did not match, so it can't be used\n", argv[i]);
            return 1;
        }
        std::printf("%s: %s, %llu events, %llu probes\n", argv[i], report.status == fr::ReplayStatus::Match ? "match" : "torn",
                    static_cast<unsigned long long>(report.events_replayed), static_cast<unsigned long long>(study.rows));
        total += study.rows;
    }
    std::fclose(out);
    std::printf("%llu probes written to %s\n", static_cast<unsigned long long>(total), argv[1]);
    return 0;
}
