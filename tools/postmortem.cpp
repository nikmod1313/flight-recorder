// Postmortem: replays recorded Real Ravi days and explains where the money went.
//   postmortem <log> [<log> ...]
//
// Each log is replayed exactly, so the report is about what really happened.
// Alongside Ravi, "shadow" strategies see the same ticks and the same recorded
// clock, as if they had traded that day instead. Their fills use the same strict
// rule, and assume their quotes changed nothing in the market.
//
// Labels and thresholds were chosen before looking at these results, so a
// label can't be picked just because it happened to fit.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include "real_ravi.h"

namespace {

using fr_ravi::RealRavi;
using fr_ravi::Settings;

constexpr uint64_t kSecond = 1'000'000'000;
constexpr int64_t kRushTrades = 20;       // trades in the previous second
constexpr int64_t kFlowAgainstBp = 3'000;  // imbalance of 0.30
constexpr int64_t kNearLimitLots = 4;
constexpr uint64_t kClusterNs = kSecond;  // previous fill under a second ago

struct Point {
    uint64_t t;
    int64_t mid2;  // twice the mid, in cents
};

struct Fill {
    uint64_t t;
    int side;  // +1 bought, -1 sold
    int64_t price;
    int64_t mid2;
    unsigned labels;
};

enum Label : unsigned { Rush = 1, AgainstFlow = 2, NearLimit = 4, Clustered = 8 };
const char* const kLabelNames[] = {"rush", "against_flow", "near_limit", "clustered"};

// Hands every strategy the time the log recorded for this event.
struct RecordedTime final : fr::Gate {
    uint64_t t = 0;
    uint64_t now_ns() override { return t; }
    uint64_t random_u64() override { return 0; }  // Ravi never asks
};

// One strategy, and everything it did over the day.
struct Trader {
    std::string name;
    RealRavi ravi;
    std::vector<Fill> fills;
    double abs_lots_sum = 0;  // for the average size of the position
    uint64_t events = 0;

    Trader(std::string n, const Settings& s) : name(std::move(n)), ravi(s) {}

    void step(const fr::Tick& tick, fr::Gate& gate, uint64_t t, unsigned labels) {
        const fr_ravi::Quotes q = ravi.quotes();
        const int64_t fills_before = ravi.fills();
        const int64_t lots_before = ravi.lots();
        ravi.on_tick(tick, gate);
        if (ravi.fills() != fills_before) {
            const int side = ravi.lots() > lots_before ? 1 : -1;
            const int64_t mid2 = tick.best_bid / fr_ravi::kUnitsPerCent + tick.best_ask / fr_ravi::kUnitsPerCent;
            fills.push_back({t, side, side > 0 ? q.bid : q.ask, mid2, labels});
        }
        abs_lots_sum += static_cast<double>(std::llabs(ravi.lots()));
        ++events;
    }
};

// Replays one log: Ravi runs through the real gate, so the replay still checks
// every event, and the shadows get the same recorded time.
struct Day final : fr::Strategy {
    Trader ravi{"ravi", Settings{}};
    std::vector<Trader> shadows;
    std::vector<Point> mids;
    std::vector<Point> trade_imbalance;  // imbalance_bp before each trade, stored in mid2
    std::deque<uint64_t> recent_trades;
    std::vector<int64_t> busyness;  // trades in the previous second, seen at each trade
    uint64_t last_fill_t = 0;

    explicit Day(const std::vector<Trader>& s) : shadows(s) {}

    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        struct Capture final : fr::Gate {
            fr::Gate& inner;
            uint64_t t = 0;
            explicit Capture(fr::Gate& g) : inner(g) {}
            uint64_t now_ns() override { return t = inner.now_ns(); }
            uint64_t random_u64() override { return inner.random_u64(); }
        } capture(gate);

        // Labels use only what Ravi knew before this tick.
        const fr_ravi::Quotes q = ravi.ravi.quotes();
        const int64_t imbalance = ravi.ravi.imbalance_bp();
        const int64_t lots_before = ravi.ravi.lots();
        const int64_t fills_before = ravi.ravi.fills();

        // Ravi first: he is the one the log recorded, and his gate read sets the time.
        const int64_t out = ravi.ravi.on_tick(tick, capture);
        const uint64_t t = capture.t;

        unsigned labels = 0;
        while (!recent_trades.empty() && recent_trades.front() + kSecond < t) recent_trades.pop_front();
        if (static_cast<int64_t>(recent_trades.size()) >= kRushTrades) labels |= Rush;
        if (std::llabs(lots_before) >= kNearLimitLots) labels |= NearLimit;
        if (last_fill_t != 0 && t - last_fill_t < kClusterNs) labels |= Clustered;

        const int64_t mid2 = tick.best_bid / fr_ravi::kUnitsPerCent + tick.best_ask / fr_ravi::kUnitsPerCent;
        if (ravi.ravi.fills() != fills_before) {
            const int side = ravi.ravi.lots() > lots_before ? 1 : -1;
            if (side * imbalance < -kFlowAgainstBp) labels |= AgainstFlow;  // sold into buying, or bought into selling
            ravi.fills.push_back({t, side, side > 0 ? q.bid : q.ask, mid2, labels});
            last_fill_t = t;
        }
        ravi.abs_lots_sum += static_cast<double>(std::llabs(ravi.ravi.lots()));
        ++ravi.events;

        RecordedTime same_time;
        same_time.t = t;
        for (Trader& s : shadows) s.step(tick, same_time, t, 0);

        if (tick.side != 0) {
            trade_imbalance.push_back({t, imbalance});
            busyness.push_back(static_cast<int64_t>(recent_trades.size()));
            recent_trades.push_back(t);
        }
        mids.push_back({t, mid2});
        return out;
    }

    uint64_t state_hash() const override { return ravi.ravi.state_hash(); }

    // The mid a set time after t, or -1 if the day ended first.
    int64_t mid2_after(uint64_t t, uint64_t wait) const {
        const uint64_t target = t + wait;
        size_t lo = 0, hi = mids.size();
        while (lo < hi) {
            const size_t m = (lo + hi) / 2;
            if (mids[m].t < target) lo = m + 1; else hi = m;
        }
        return lo == mids.size() ? -1 : mids[lo].mid2;
    }
};

// Totals across every day, so a label is judged on all fills together.
struct Tally {
    long count = 0;
    double markout = 0;  // cents per BTC, positive is good for the trader
    long against = 0;
    void add(double m) { ++count; markout += m; against += m < 0; }
    double average() const { return count ? markout / count : 0; }
};

struct Totals {
    double gross = 0, fees = 0, captured = 0, abs_lots = 0;
    long fills = 0, events = 0, max_lots = 0;
    Tally one_second, five_seconds;
};

void score(const Day& day, const Trader& trader, Totals& totals, Tally* by_label, Tally* without_label) {
    totals.gross += trader.ravi.gross_pnl() / 1e5;
    totals.fees += trader.ravi.fees() / 1e5;
    totals.fills += static_cast<long>(trader.fills.size());
    totals.events += static_cast<long>(trader.events);
    totals.abs_lots += trader.abs_lots_sum;
    if (trader.ravi.max_lots() > totals.max_lots) totals.max_lots = trader.ravi.max_lots();
    for (const Fill& f : trader.fills) {
        totals.captured += f.side * (f.mid2 / 2.0 - static_cast<double>(f.price)) / 1e5;
        const int64_t later1 = day.mid2_after(f.t, kSecond);
        const int64_t later5 = day.mid2_after(f.t, 5 * kSecond);
        if (later5 >= 0) totals.five_seconds.add(f.side * (later5 - f.mid2) / 2.0);
        if (later1 < 0) continue;
        const double m = f.side * (later1 - f.mid2) / 2.0;
        totals.one_second.add(m);
        if (!by_label) continue;
        for (unsigned i = 0; i < 4; ++i) (f.labels & (1u << i) ? by_label : without_label)[i].add(m);
    }
}

void print_totals(const std::string& name, const Totals& t) {
    std::printf("%-12s fills=%-4ld gross_usd=%8.4f fees_usd=%7.4f net_usd=%8.4f spread_usd=%7.4f "
                "price_moves_usd=%8.4f avg_abs_lots=%.2f most_lots=%ld\n",
                name.c_str(), t.fills, t.gross, t.fees, t.gross - t.fees, t.captured, t.gross - t.captured,
                t.events ? t.abs_lots / t.events : 0, t.max_lots);
    std::printf("%-12s markout_1s=%8.2f (against %ld of %ld)  markout_5s=%8.2f (against %ld of %ld)  cents per BTC\n",
                "", t.one_second.average(), t.one_second.against, t.one_second.count, t.five_seconds.average(),
                t.five_seconds.against, t.five_seconds.count);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: postmortem <log> [<log> ...]\n");
        return 64;
    }

    // Shadows: what if Ravi had been simpler? Each removes one idea.
    Settings plain;                                      // a plain stall: same typical width, nothing clever
    plain.risk_divisor = std::numeric_limits<int64_t>::max();  // no inventory slide, no volatility widening
    plain.lean_cents = 0;
    plain.base_spread_cents = 255;  // Ravi's median width on the first live day
    Settings no_lean;
    no_lean.lean_cents = 0;
    // The three fixes, each set from days 1-4 only, before days 5-7 were recorded.
    Settings step_aside;
    step_aside.step_aside_bp = 3'000;  // the same 0.30 the labels use
    Settings lean_300;
    lean_300.lean_cents = 300;  // about the $3.50 move the signal predicts
    Settings rush_pull;
    rush_pull.rush_trades = 217;  // the busiest 10% of moments on days 1-4
    Settings all_three = step_aside;
    all_three.lean_cents = lean_300.lean_cents;
    all_three.rush_trades = rush_pull.rush_trades;
    const std::vector<Trader> shadow_set = {Trader("plain_255", plain), Trader("no_lean", no_lean),
                                            Trader("step_aside", step_aside), Trader("lean_300", lean_300),
                                            Trader("rush_pull", rush_pull), Trader("all_three", all_three)};

    Totals ravi_totals;
    std::vector<Totals> shadow_totals(shadow_set.size());
    Tally with_label[4], without_label[4];
    Tally flow_up, flow_down, flow_middle;  // next-second mid move after a trade, by imbalance
    double sum_xy = 0, sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0;
    long pairs = 0;
    std::vector<int64_t> busyness;

    for (int i = 1; i < argc; ++i) {
        Day day(shadow_set);
        const fr::ReplayReport report = fr::replay_run(day, argv[i]);
        // A torn tail means the feed died: every event before the tear still replayed exactly.
        if (report.status == fr::ReplayStatus::TornTail) {
            std::printf("%s: torn, so only the %llu events before the tear are used\n", argv[i],
                        static_cast<unsigned long long>(report.events_replayed));
        } else if (report.status != fr::ReplayStatus::Match) {
            std::fprintf(stderr, "%s: replay did not match, so it can't be analysed\n", argv[i]);
            return 1;
        }
        const Trader& r = day.ravi;
        std::printf("%s: events=%llu fills=%zu gross_usd=%.5f fees_usd=%.5f net_usd=%.5f\n", argv[i],
                    static_cast<unsigned long long>(report.events_replayed), r.fills.size(), r.ravi.gross_pnl() / 1e5,
                    r.ravi.fees() / 1e5, r.ravi.net_pnl() / 1e5);
        score(day, r, ravi_totals, with_label, without_label);
        busyness.insert(busyness.end(), day.busyness.begin(), day.busyness.end());
        for (size_t s = 0; s < day.shadows.size(); ++s) score(day, day.shadows[s], shadow_totals[s], nullptr, nullptr);

        // The signal test: does imbalance before a trade predict the next second's move?
        for (const Point& p : day.trade_imbalance) {
            const int64_t before = day.mid2_after(p.t, 0);
            const int64_t later = day.mid2_after(p.t, kSecond);
            if (before < 0 || later < 0) continue;
            const double move = (later - before) / 2.0;
            const double x = p.mid2 / 10'000.0;
            (p.mid2 > kFlowAgainstBp ? flow_up : p.mid2 < -kFlowAgainstBp ? flow_down : flow_middle).add(move);
            sum_x += x; sum_y += move; sum_xy += x * move; sum_xx += x * x; sum_yy += move * move;
            ++pairs;
        }
    }

    std::printf("\n== All days: where the money went (USD; trades are paper fills) ==\n");
    print_totals("ravi", ravi_totals);
    for (size_t s = 0; s < shadow_set.size(); ++s) print_totals(shadow_set[s].name, shadow_totals[s]);

    std::printf("\n== Ravi's fills by label: average move 1 s later, cents per BTC ==\n");
    for (unsigned i = 0; i < 4; ++i) {
        std::printf("%-13s with: %4ld fills, %8.2f (against %ld)   without: %4ld fills, %8.2f (against %ld)\n",
                    kLabelNames[i], with_label[i].count, with_label[i].average(), with_label[i].against,
                    without_label[i].count, without_label[i].average(), without_label[i].against);
    }

    std::printf("\n== Signal: mid move 1 s after a trade, by imbalance before it, cents per BTC ==\n");
    std::printf("buyers pushing (> +0.30):  %7ld trades, average move %+.3f\n", flow_up.count, flow_up.average());
    std::printf("balanced:                  %7ld trades, average move %+.3f\n", flow_middle.count, flow_middle.average());
    std::printf("sellers pushing (< -0.30): %7ld trades, average move %+.3f\n", flow_down.count, flow_down.average());
    const double n = static_cast<double>(pairs);
    const double corr = (n * sum_xy - sum_x * sum_y) /
                        std::sqrt((n * sum_xx - sum_x * sum_x) * (n * sum_yy - sum_y * sum_y));
    std::printf("correlation of imbalance with the next-second move: %.3f over %ld trades (windows overlap)\n", corr, pairs);

    std::sort(busyness.begin(), busyness.end());
    auto pct = [&](size_t p) { return busyness.empty() ? 0 : busyness[busyness.size() * p / 100]; };
    std::printf("\n== Trades in the second before each trade: median %lld, 90th percentile %lld, 99th %lld ==\n",
                static_cast<long long>(pct(50)), static_cast<long long>(pct(90)), static_cast<long long>(pct(99)));
    return 0;
}
