// Runs one record or replay in its own process, as a real replay would.
//   fr_tool record <strategy> <log> <events> <at> [max_file_bytes]
//   fr_tool replay <strategy> <log> <at>
//   fr_tool record-stdin <strategy> <log>
//   fr_tool fork <strategy> <log> <fork_at> <at> [setting=value ...]
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
void print_day(const fr::Strategy& strategy, const char* name = "ravi") {
    const auto* ravi = dynamic_cast<const fr_ravi::RealRavi*>(&strategy);
    if (!ravi) return;
    std::printf("%s fills=%lld lots=%lld most_lots=%lld gross_usd=%.5f fees_usd=%.5f net_usd=%.5f stopped=%d\n", name,
                static_cast<long long>(ravi->fills()), static_cast<long long>(ravi->lots()),
                static_cast<long long>(ravi->max_lots()), static_cast<double>(ravi->gross_pnl()) / 100'000.0,
                static_cast<double>(ravi->fees()) / 100'000.0, static_cast<double>(ravi->net_pnl()) / 100'000.0,
                ravi->stopped() ? 1 : 0);
}

int usage();

const char* fork_status_name(fr::ForkStatus status) {
    switch (status) {
        case fr::ForkStatus::Done: return "done";
        case fr::ForkStatus::TornTail: return "torn";
        case fr::ForkStatus::NeverForked: return "never_forked";
        case fr::ForkStatus::Diverged: return "diverged";
        case fr::ForkStatus::Corrupt: return "corrupt";
        case fr::ForkStatus::NeedsMoreInputs: return "needs_more_inputs";
    }
    return "unknown";
}

// Applies "name=value" changes to Ravi's settings. Throws on a name it doesn't know,
// so a typo can never fork into an unchanged strategy that looks like a result.
fr_ravi::Settings changed_settings(fr_ravi::Settings s, int count, char** changes) {
    for (int i = 0; i < count; ++i) {
        const std::string change = changes[i];
        const size_t eq = change.find('=');
        if (eq == std::string::npos) throw std::runtime_error("change must be name=value: " + change);
        const std::string name = change.substr(0, eq);
        const int64_t value = std::stoll(change.substr(eq + 1));
        if (name == "lean_cents") s.lean_cents = value;
        else if (name == "base_spread_cents") s.base_spread_cents = value;
        else if (name == "ladder_cents") s.ladder_cents = value;
        else if (name == "chase_cents") s.chase_cents = value;
        else if (name == "step_aside_bp") s.step_aside_bp = value;
        else if (name == "rush_trades") s.rush_trades = value;
        else if (name == "max_inventory_lots") s.max_inventory_lots = value;
        else throw std::runtime_error("unknown setting: " + name);
    }
    return s;
}

int fork(int argc, char** argv) {
    const uint64_t fork_at = std::strtoull(argv[4], nullptr, 10);
    const uint64_t at = std::strtoull(argv[5], nullptr, 10);
    // The fork is built as the replaying side, so a test strategy can make only the fork ask for more.
    auto original = fr_test::make_strategy(argv[2], at, false);
    auto fork = fr_test::make_strategy(argv[2], at, true);
    if (!original || !fork) return usage();
    std::function<void(fr::Strategy&)> change = [](fr::Strategy&) {};
    if (argc > 6) {
        auto* ravi = dynamic_cast<fr_ravi::RealRavi*>(fork.get());
        if (!ravi) throw std::runtime_error("only Ravi has settings to change");
        const fr_ravi::Settings next = changed_settings(ravi->settings(), argc - 6, argv + 6);
        change = [next](fr::Strategy& s) { static_cast<fr_ravi::RealRavi&>(s).change_settings(next); };
    }
    const fr::ForkReport report = fr::fork_run(*original, *fork, fork_at, change, argv[3]);
    auto seq = [](const std::optional<uint64_t>& v) { return v ? static_cast<long long>(*v) : -1LL; };
    std::printf("fork status=%s events=%llu fork_at=%llu first_difference=%lld different=%llu first=%lld needs_input_at=%lld\n",
                fork_status_name(report.status), static_cast<unsigned long long>(report.events),
                static_cast<unsigned long long>(fork_at), seq(report.first_difference_seq),
                static_cast<unsigned long long>(report.different_events), seq(report.first_divergence_seq),
                seq(report.fork_needed_more_inputs_seq));
    print_day(*original, "original");
    print_day(*fork, "forked");
    return 0;
}

int usage() {
    std::fprintf(stderr, "usage: fr_tool record|replay|fork <strategy> <log> ...\n");
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
    if (argc >= 6 && std::string(argv[1]) == "fork") return fork(argc, argv);
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
