// What recording adds per event. Stamps each on_tick entry, so a gap between
// stamps is one full loop: feed, gate, strategy, and recording if it is on.
// Results are synthetic and only describe the machine they ran on.
#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "feed.h"
#include "strategies.h"

namespace {

constexpr uint64_t kEvents = 1'000'000;
constexpr int kRounds = 5;

class Stamped final : public fr_test::Clean {
public:
    explicit Stamped(std::vector<uint64_t>& stamps) : stamps_(stamps) {}
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        stamps_[tick.seq] = mach_absolute_time();
        return Clean::on_tick(tick, gate);
    }

private:
    std::vector<uint64_t>& stamps_;
};

class DirectGate final : public fr::Gate {
public:
    uint64_t now_ns() override {
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    uint64_t random_u64() override { return rng_(); }

private:
    std::mt19937_64 rng_{std::random_device{}()};
};

struct Percentiles {
    double p50, p99, p999, max;
};

Percentiles summarize(const std::vector<uint64_t>& stamps) {
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    std::vector<double> gaps;
    gaps.reserve(stamps.size());
    for (size_t i = 1; i < stamps.size(); ++i) {
        gaps.push_back(static_cast<double>(stamps[i] - stamps[i - 1]) * base.numer / base.denom);
    }
    std::sort(gaps.begin(), gaps.end());
    auto at = [&](double q) { return gaps[static_cast<size_t>(q * static_cast<double>(gaps.size() - 1))]; };
    return {at(0.50), at(0.99), at(0.999), gaps.back()};
}

// How many one-event gaps were slow.
void print_slow_gaps(const char* name, const std::vector<uint64_t>& stamps) {
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    uint64_t over_1us = 0, over_10us = 0, over_100us = 0;
    for (size_t i = 1; i < stamps.size(); ++i) {
        const double gap = static_cast<double>(stamps[i] - stamps[i - 1]) * base.numer / base.denom;
        over_1us += gap > 1'000;
        over_10us += gap > 10'000;
        over_100us += gap > 100'000;
    }
    std::printf("%-9s slow gaps: %6llu over 1 us, %6llu over 10 us, %6llu over 100 us\n", name,
                static_cast<unsigned long long>(over_1us), static_cast<unsigned long long>(over_10us),
                static_cast<unsigned long long>(over_100us));
}

Percentiles run_baseline(std::vector<uint64_t>& stamps) {
    fr_test::SyntheticFeed feed(kEvents);
    Stamped strategy(stamps);
    DirectGate gate;
    fr::Tick tick;
    while (feed.next(tick)) strategy.on_tick(tick, gate);
    return summarize(stamps);
}

Percentiles run_recording(std::vector<uint64_t>& stamps, const std::filesystem::path& log) {
    std::filesystem::remove(log);
    fr_test::SyntheticFeed feed(kEvents);
    Stamped strategy(stamps);
    const fr::RecordReport report = fr::record_run(feed, strategy, log.string());
    std::printf("ring full: %llu waits, disk syncs: %llu, slowest sync: %llu ns\n",
                static_cast<unsigned long long>(report.ring_full_waits),
                static_cast<unsigned long long>(report.disk_syncs),
                static_cast<unsigned long long>(report.max_sync_ns));
    Percentiles result = summarize(stamps);
    std::filesystem::remove(log);
    return result;
}

void print(const char* name, int round, const Percentiles& p) {
    std::printf("%-9s round %d  p50 %8.0f ns  p99 %8.0f ns  p99.9 %8.0f ns  max %10.0f ns\n", name, round, p.p50,
                p.p99, p.p999, p.max);
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path build = argc > 1 ? argv[1] : "build";
    const std::filesystem::path log = build / "bench.log";
    std::vector<uint64_t> stamps(kEvents, 0);  // touched up front so page faults stay out of the timing

    std::printf("SYNTHETIC: %llu made-up events per round. Timer tick is about 42 ns on Apple silicon.\n",
                static_cast<unsigned long long>(kEvents));
    std::vector<double> added_p99, added_p999, worst_base, worst_rec;
    for (int round = 1; round <= kRounds; ++round) {
        const Percentiles base = run_baseline(stamps);
        print("baseline", round, base);
        print_slow_gaps("baseline", stamps);
        const Percentiles rec = run_recording(stamps, log);
        print("recording", round, rec);
        print_slow_gaps("recording", stamps);
        added_p99.push_back(rec.p99 - base.p99);
        added_p999.push_back(rec.p999 - base.p999);
        worst_base.push_back(base.max);
        worst_rec.push_back(rec.max);
    }
    auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    const double p99 = median(added_p99), p999 = median(added_p999);
    std::printf("median added by recording: p99 %.0f ns, p99.9 %.0f ns (kill line: 1000 ns each)\n", p99, p999);
    std::printf("median worst event: %.0f ns recording, %.0f ns baseline\n", median(worst_rec), median(worst_base));
    std::printf("fast kill test: %s\n", p99 < 1000 && p999 < 1000 ? "PASS" : "FAIL");
    return 0;
}
