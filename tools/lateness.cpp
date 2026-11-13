// Lateness: how late do trades reach us, and is it worse in a rush?
//   lateness <log> [<log> ...]
//
// Each trade carries Binance's own time (exchange_ns, since 1970, 1 ms resolution).
// Our arrival time comes from the machine's steady clock, which counts from boot, so
// the two can't be subtracted directly. But their gap is a fixed offset plus the
// delay. Taking the smallest gap seen in each minute as "no delay" leaves the extra
// delay of every trade: how much later it arrived than the fastest trade around it.
// Using a minute rather than the whole log keeps slow clock drift out of the answer.
#include <algorithm>
#include <cstdio>
#include <deque>
#include <vector>

#include "real_ravi.h"

namespace {

constexpr int64_t kSecond = 1'000'000'000;
constexpr int64_t kMinute = 60 * kSecond;

struct Trade {
    int64_t arrival;
    int64_t gap;  // arrival - exchange time: a fixed offset plus the delay
    int busy;     // trades that arrived in the second before
};

struct Lateness final : fr::Strategy {
    fr_ravi::RealRavi ravi;  // the strategy the log was recorded with, so replay is checked
    std::vector<Trade> trades;
    std::deque<int64_t> last_second;

    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        struct Capture final : fr::Gate {
            fr::Gate& inner;
            uint64_t t = 0;
            explicit Capture(fr::Gate& g) : inner(g) {}
            uint64_t now_ns() override { return t = inner.now_ns(); }
            uint64_t random_u64() override { return inner.random_u64(); }
        } capture(gate);
        const int64_t out = ravi.on_tick(tick, capture);
        const int64_t t = static_cast<int64_t>(capture.t);
        if (tick.side != 0 && tick.exchange_ns > 0) {
            while (!last_second.empty() && last_second.front() + kSecond < t) last_second.pop_front();
            trades.push_back({t, t - tick.exchange_ns, static_cast<int>(last_second.size())});
            last_second.push_back(t);
        }
        return out;
    }
    void on_book(const fr::Book& book) override { ravi.on_book(book); }
    uint64_t state_hash() const override { return ravi.state_hash(); }
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: lateness <log> [<log> ...]\n");
        return 64;
    }
    std::vector<double> all, calm, rush;  // extra delay in ms
    std::vector<int> busy;
    for (int i = 1; i < argc; ++i) {
        Lateness day;
        const fr::ReplayReport report = fr::replay_run(day, argv[i]);
        if (report.status != fr::ReplayStatus::Match && report.status != fr::ReplayStatus::TornTail) {
            std::fprintf(stderr, "%s: replay did not match, so it can't be used\n", argv[i]);
            return 1;
        }
        if (day.trades.empty()) {
            std::printf("%s: no trades with exchange times (recorded before FRLOG03?)\n", argv[i]);
            continue;
        }
        // The smallest gap within a minute either side is taken as "no delay".
        size_t lo = 0, hi = 0;
        std::deque<size_t> window;  // indexes, gaps increasing: a sliding minimum
        std::vector<double> day_delays;
        for (size_t k = 0; k < day.trades.size(); ++k) {
            const int64_t t = day.trades[k].arrival;
            while (hi < day.trades.size() && day.trades[hi].arrival <= t + kMinute) {
                while (!window.empty() && day.trades[window.back()].gap >= day.trades[hi].gap) window.pop_back();
                window.push_back(hi++);
            }
            while (day.trades[lo].arrival < t - kMinute) ++lo;
            while (window.front() < lo) window.pop_front();
            const double delay_ms = (day.trades[k].gap - day.trades[window.front()].gap) / 1e6;
            day_delays.push_back(delay_ms);
            busy.push_back(day.trades[k].busy);
        }
        std::printf("%s: %zu trades, extra delay median %.1f ms, p90 %.1f ms, p99 %.1f ms, worst %.1f ms\n", argv[i],
                    day_delays.size(), percentile(day_delays, 0.5), percentile(day_delays, 0.9),
                    percentile(day_delays, 0.99), percentile(day_delays, 1.0));
        all.insert(all.end(), day_delays.begin(), day_delays.end());
    }
    if (all.empty()) return 0;
    // Rush: the busiest 10% of moments, by trades in the second before.
    std::vector<int> sorted_busy = busy;
    std::sort(sorted_busy.begin(), sorted_busy.end());
    const int rush_line = sorted_busy[sorted_busy.size() * 9 / 10];
    for (size_t k = 0; k < all.size(); ++k) (busy[k] >= rush_line ? rush : calm).push_back(all[k]);
    std::printf("\nall:   %zu trades, extra delay median %.1f ms, p90 %.1f ms, p99 %.1f ms\n", all.size(),
                percentile(all, 0.5), percentile(all, 0.9), percentile(all, 0.99));
    std::printf("calm:  %zu trades, extra delay median %.1f ms, p90 %.1f ms, p99 %.1f ms\n", calm.size(),
                percentile(calm, 0.5), percentile(calm, 0.9), percentile(calm, 0.99));
    std::printf("rush (%d+ trades in the second before): %zu trades, extra delay median %.1f ms, p90 %.1f ms, p99 %.1f ms\n",
                rush_line, rush.size(), percentile(rush, 0.5), percentile(rush, 0.9), percentile(rush, 0.99));
    std::printf("exchange times have 1 ms resolution, so delays under about 1 ms can't be told apart\n");
    return 0;
}
