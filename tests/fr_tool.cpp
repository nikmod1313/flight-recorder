// Runs one record or replay in its own process, as a real replay would.
//   fr_tool record <strategy> <log> <events> <at> [max_file_bytes]
//   fr_tool replay <strategy> <log> <at>
//   fr_tool record-stdin <strategy> <log>
#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

#include "feed.h"
#include "strategies.h"

namespace {

const char* status_name(fr::ReplayStatus status) {
    switch (status) {
        case fr::ReplayStatus::Match: return "match";
        case fr::ReplayStatus::Diverged: return "diverged";
        case fr::ReplayStatus::TornTail: return "torn";
        case fr::ReplayStatus::Corrupt: return "corrupt";
    }
    return "unknown";
}

// Real Ravi's day, in dollars. A replay must print exactly the same line.
void print_day(const fr::Strategy& strategy) {
    const auto* ravi = dynamic_cast<const fr_ravi::RealRavi*>(&strategy);
    if (!ravi) return;
    std::printf("ravi fills=%lld lots=%lld most_lots=%lld gross_usd=%.5f fees_usd=%.5f net_usd=%.5f stopped=%d\n",
                static_cast<long long>(ravi->fills()), static_cast<long long>(ravi->lots()),
                static_cast<long long>(ravi->max_lots()), static_cast<double>(ravi->gross_pnl()) / 100'000.0,
                static_cast<double>(ravi->fees()) / 100'000.0, static_cast<double>(ravi->net_pnl()) / 100'000.0,
                ravi->stopped() ? 1 : 0);
}

int usage() {
    std::fprintf(stderr, "usage: fr_tool record|replay <strategy> <log> ...\n");
    return 64;
}

int run(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "record-stdin") {
        alarm(0);  // a live recording runs as long as its feed does
        auto live = fr_test::make_strategy(argv[2], 0, false);
        if (!live) return usage();
        fr_test::LineFeed feed;
        fr::RecordReport report = fr::record_run(feed, *live, argv[3]);
        std::printf("events=%llu ring_full_waits=%llu disk_syncs=%llu max_sync_ns=%llu\n",
                    static_cast<unsigned long long>(report.events),
                    static_cast<unsigned long long>(report.ring_full_waits),
                    static_cast<unsigned long long>(report.disk_syncs),
                    static_cast<unsigned long long>(report.max_sync_ns));
        print_day(*live);
        return 0;
    }
    if (argc < 5) return usage();
    const std::string mode = argv[1];
    const bool replaying = mode == "replay";
    if (!replaying && (mode != "record" || argc < 6)) return usage();

    const uint64_t at = std::strtoull(argv[replaying ? 4 : 5], nullptr, 10);
    auto strategy = fr_test::make_strategy(argv[2], at, replaying);
    if (!strategy) return usage();

    if (replaying) {
        fr::ReplayReport report = fr::replay_run(*strategy, argv[3]);
        std::printf("status=%s events=%llu first=%lld\n", status_name(report.status),
                    static_cast<unsigned long long>(report.events_replayed),
                    report.first_divergence_seq ? static_cast<long long>(*report.first_divergence_seq) : -1);
        print_day(*strategy);
        return 0;
    }

    if (argc > 6) {  // makes the OS refuse writes past this size, like a full disk
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit{};
        limit.rlim_cur = limit.rlim_max = std::strtoull(argv[6], nullptr, 10);
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::perror("setrlimit");
            return 3;
        }
    }
    fr_test::SyntheticFeed feed(std::strtoull(argv[4], nullptr, 10));
    fr::RecordReport report = fr::record_run(feed, *strategy, argv[3]);
    std::printf("events=%llu\n", static_cast<unsigned long long>(report.events));
    print_day(*strategy);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    alarm(120);  // a hang must fail a test, not stall it forever
    try {
        return run(argc, argv);
    } catch (const std::logic_error& error) {  // a stub must never pass as a clean refusal
        std::fprintf(stderr, "bug: %s\n", error.what());
        return 70;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}
