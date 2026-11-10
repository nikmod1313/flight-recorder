// The worth-it test: does forking from a real moment give a different answer
// than a backtest that starts fresh at that same moment?
//   worth_it <log> [<log> ...]
//
// At 20%, 40%, 60% and 80% of each day, one change ("lean $3 instead of $1") is
// judged two ways, from that moment to the end of the day:
//   fork:  Ravi as he really was at that moment (stock, volatility, crowd reading),
//          with and without the change. This is what fr::fork_run does.
//   fresh: a brand-new Ravi starting at that moment with nothing, with and
//          without the change. This is a normal backtest started there.
// Each gives an effect: profit with the change minus profit without it. If the
// two effects usually agree, forking adds little over a fresh backtest.
// Both are also measured over just the first 60 seconds, when the fresh start
// hasn't caught up with what the real Ravi already knew.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "real_ravi.h"

namespace {

using fr_ravi::RealRavi;
using fr_ravi::Settings;

struct RecordedTime final : fr::Gate {
    uint64_t t = 0;
    uint64_t now_ns() override { return t; }
    uint64_t random_u64() override { return 0; }  // Ravi never asks
};

// The four Ravis started at one moment, and how they did from there.
struct Point {
    uint64_t at = 0;  // event number
    uint64_t t_at = 0;
    int64_t base_gross_at = 0;  // the real Ravi's profit at that moment, so his later profit counts from there
    int64_t lots_at = 0;
    std::unique_ptr<RealRavi> forked, fresh_plain, fresh_changed;
    uint64_t last_disagreement_t = 0;  // last time the fork and the fresh start quoted differently
    uint64_t disagreements = 0;
    bool snapped = false;  // profits 60 seconds after the moment
    int64_t base_60 = 0, forked_60 = 0, fresh_plain_60 = 0, fresh_changed_60 = 0;
};

constexpr uint64_t kWindowNs = 60'000'000'000;

struct Day final : fr::Strategy {
    RealRavi ravi;  // as recorded: checked against the log on every event
    Settings changed;
    std::vector<Point> points;
    uint64_t events = 0, last_t = 0;

    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        struct Capture final : fr::Gate {
            fr::Gate& inner;
            uint64_t t = 0;
            explicit Capture(fr::Gate& g) : inner(g) {}
            uint64_t now_ns() override { return t = inner.now_ns(); }
            uint64_t random_u64() override { return inner.random_u64(); }
        } capture(gate);

        for (Point& p : points) {
            if (p.at != events) continue;
            p.forked = std::make_unique<RealRavi>(ravi);  // the real Ravi, copied mid-day
            p.forked->change_settings(changed);
            p.fresh_plain = std::make_unique<RealRavi>();
            p.fresh_changed = std::make_unique<RealRavi>(changed);
            p.base_gross_at = ravi.gross_pnl();
            p.lots_at = ravi.lots();
        }
        const int64_t out = ravi.on_tick(tick, capture);
        RecordedTime same_time;
        same_time.t = last_t = capture.t;
        for (Point& p : points) {
            if (!p.forked) continue;
            if (p.t_at == 0) p.t_at = capture.t;
            const int64_t fork_out = p.forked->on_tick(tick, same_time);
            p.fresh_plain->on_tick(tick, same_time);
            if (p.fresh_changed->on_tick(tick, same_time) != fork_out) {
                p.last_disagreement_t = capture.t;
                ++p.disagreements;
            }
            if (!p.snapped && capture.t >= p.t_at + kWindowNs) {
                p.snapped = true;
                p.base_60 = ravi.gross_pnl();
                p.forked_60 = p.forked->gross_pnl();
                p.fresh_plain_60 = p.fresh_plain->gross_pnl();
                p.fresh_changed_60 = p.fresh_changed->gross_pnl();
            }
        }
        ++events;
        return out;
    }
    uint64_t state_hash() const override { return ravi.state_hash(); }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: worth_it <log> [<log> ...]\n");
        return 64;
    }
    Settings changed;
    changed.lean_cents = 300;

    struct Row { double fork_effect, fresh_effect, fork_pnl, fresh_pnl, washout_s; int64_t lots_at; bool has_60; double fork_60, fresh_60; };
    std::vector<Row> rows;
    std::printf("%-16s %9s %6s %10s %10s %10s %9s\n", "log", "at", "lots", "fork_usd", "fresh_usd", "gap_usd", "washout_s");
    for (int i = 1; i < argc; ++i) {
        // A first pass counts the events, so the moments can be placed at fixed fractions of the day.
        struct Count final : fr::Strategy {
            RealRavi ravi;
            int64_t on_tick(const fr::Tick& t, fr::Gate& g) override { return ravi.on_tick(t, g); }
            uint64_t state_hash() const override { return ravi.state_hash(); }
        } count;
        const fr::ReplayReport first = fr::replay_run(count, argv[i]);
        if (first.status != fr::ReplayStatus::Match && first.status != fr::ReplayStatus::TornTail) {
            std::fprintf(stderr, "%s: replay did not match, so it can't be used\n", argv[i]);
            return 1;
        }
        Day day;
        day.changed = changed;
        for (int k = 1; k <= 4; ++k) {
            day.points.emplace_back();
            day.points.back().at = first.events_replayed * k / 5;
        }
        const fr::ReplayReport report = fr::replay_run(day, argv[i]);
        if (report.events_replayed != first.events_replayed) {
            std::fprintf(stderr, "%s: the second replay differed from the first\n", argv[i]);
            return 1;
        }
        for (const Point& p : day.points) {
            const double base_from = (day.ravi.gross_pnl() - p.base_gross_at) / 1e5;  // the real Ravi, unchanged
            const double fork_effect = (p.forked->gross_pnl() - p.base_gross_at) / 1e5 - base_from;
            const double fresh_effect = (p.fresh_changed->gross_pnl() - p.fresh_plain->gross_pnl()) / 1e5;
            const double washout = p.disagreements ? (p.last_disagreement_t - p.t_at) / 1e9 : 0;
            rows.push_back({fork_effect, fresh_effect, (p.forked->gross_pnl() - p.base_gross_at) / 1e5,
                            p.fresh_changed->gross_pnl() / 1e5, washout, p.lots_at, p.snapped,
                            (p.forked_60 - p.base_60) / 1e5, (p.fresh_changed_60 - p.fresh_plain_60) / 1e5});
            std::printf("%-16s %9llu %6lld %+10.5f %+10.5f %+10.5f %9.1f\n", argv[i], static_cast<unsigned long long>(p.at),
                        static_cast<long long>(p.lots_at), fork_effect, fresh_effect, fork_effect - fresh_effect, washout);
        }
    }

    const double n = static_cast<double>(rows.size());
    double abs_fork = 0, abs_gap = 0, abs_pnl_gap = 0;
    int same_sign = 0, held = 0, held_same_sign = 0;
    std::vector<double> washouts;
    for (const Row& r : rows) {
        abs_fork += std::fabs(r.fork_effect);
        abs_gap += std::fabs(r.fork_effect - r.fresh_effect);
        abs_pnl_gap += std::fabs(r.fork_pnl - r.fresh_pnl);
        const bool agree = (r.fork_effect > 0) == (r.fresh_effect > 0);
        same_sign += agree;
        if (r.lots_at != 0) {
            ++held;
            held_same_sign += agree;
        }
        washouts.push_back(r.washout_s);
    }
    std::sort(washouts.begin(), washouts.end());
    double abs_fork_60 = 0, abs_gap_60 = 0;
    int n60 = 0, same_60 = 0, moved_60 = 0;
    for (const Row& r : rows) {
        if (!r.has_60) continue;
        ++n60;
        abs_fork_60 += std::fabs(r.fork_60);
        abs_gap_60 += std::fabs(r.fork_60 - r.fresh_60);
        if (r.fork_60 != 0 || r.fresh_60 != 0) {
            ++moved_60;
            same_60 += (r.fork_60 > 0) == (r.fresh_60 > 0) && (r.fork_60 != 0) == (r.fresh_60 != 0);
        }
    }
    std::printf("\nmoments=%zu  average size of the change's effect (fork) = $%.4f\n", rows.size(), abs_fork / n);
    std::printf("average gap between fork and fresh answers about the change = $%.4f (%.0f%% of the effect)\n", abs_gap / n,
                100 * abs_gap / abs_fork);
    std::printf("average gap in the changed Ravi's own profit, fork vs fresh = $%.4f\n", abs_pnl_gap / n);
    std::printf("fork and fresh agree on whether the change helped: %d of %zu (%d of %d when Ravi held stock at that moment)\n",
                same_sign, rows.size(), held_same_sign, held);
    std::printf("first 60 s only: average effect $%.4f, average gap $%.4f (%.0f%% of the effect), same answer %d of %d moments where either changed\n",
                abs_fork_60 / n60, abs_gap_60 / n60, abs_fork_60 > 0 ? 100 * abs_gap_60 / abs_fork_60 : 0.0, same_60, moved_60);
    std::printf("time until the fresh start quotes like the fork for good: median %.1f s, longest %.1f s\n",
                washouts[washouts.size() / 2], washouts.back());
    return 0;
}
