// Holes: list the stretches where a recording was frozen (see holes.h).
//   holes <log> [<log> ...]
// Prints each hole with the event it ends at and how far each clock moved across it,
// then a total. Exits 0 whether or not holes are found; 1 if a log doesn't replay.
#include <cstdio>

#include "holes.h"
#include "real_ravi.h"

namespace {

struct Scan final : fr::Strategy {
    fr_ravi::RealRavi ravi;  // the strategy the log was recorded with, so replay is checked
    fr_holes::HoleFinder finder;
    uint64_t trades = 0;
    int holes = 0;
    int64_t missing_ns = 0;  // market time lost inside holes
    int64_t covered_ns = 0;  // market time watched: steps between trades that are not holes
    bool any = false;
    const char* name = "";

    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        struct Capture final : fr::Gate {
            fr::Gate& inner;
            uint64_t t = 0;
            explicit Capture(fr::Gate& g) : inner(g) {}
            uint64_t now_ns() override { return t = inner.now_ns(); }
            uint64_t random_u64() override { return inner.random_u64(); }
        } capture(gate);
        const int64_t out = ravi.on_tick(tick, capture);
        if (tick.side != 0 && tick.exchange_ns > 0) {
            ++trades;
            const bool hole = finder.on_trade(static_cast<int64_t>(capture.t), tick.exchange_ns);
            if (any && !hole) covered_ns += finder.last().exchange_jump;
            any = true;
            if (hole) {
                const fr_holes::Hole h = finder.last();
                ++holes;
                missing_ns += h.exchange_jump;
                std::printf("%s: hole before event %llu: our clock moved %.1f s, Binance's moved %.1f s\n", name,
                            static_cast<unsigned long long>(tick.seq), h.arrival_jump / 1e9, h.exchange_jump / 1e9);
            }
        }
        return out;
    }
    void on_book(const fr::Book& book) override { ravi.on_book(book); }
    uint64_t state_hash() const override { return ravi.state_hash(); }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: holes <log> [<log> ...]\n");
        return 64;
    }
    int total = 0;
    double missing = 0;
    double watched = 0;  // minutes
    for (int i = 1; i < argc; ++i) {
        Scan scan;
        scan.name = argv[i];
        const fr::ReplayReport report = fr::replay_run(scan, argv[i]);
        if (report.status != fr::ReplayStatus::Match && report.status != fr::ReplayStatus::TornTail) {
            std::fprintf(stderr, "%s: replay did not match, so it can't be checked\n", argv[i]);
            return 1;
        }
        if (scan.trades == 0) {
            std::printf("%s: no trades with exchange times, so holes can't be found\n", argv[i]);
            continue;
        }
        std::printf("%s: %llu trades, %.1f min of market watched, %d holes, %.1f min of market missing\n", argv[i],
                    static_cast<unsigned long long>(scan.trades), scan.covered_ns / 6e10, scan.holes, scan.missing_ns / 6e10);
        watched += scan.covered_ns / 6e10;
        total += scan.holes;
        missing += scan.missing_ns / 6e10;
    }
    std::printf("total: %.1f h of market watched, %d holes, %.1f min of market missing\n", watched / 60, total, missing);
    return 0;
}
