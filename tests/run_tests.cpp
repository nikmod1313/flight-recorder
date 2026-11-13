// Black-box tests. Each record and replay is a separate fr_tool process.
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "fr/flight_recorder.h"

namespace fs = std::filesystem;

namespace {

struct Result {
    int exit_code = -1;
    std::string out;
};

struct Replay {
    int exit_code = -1;
    std::string status;
    long long events = -1;
    long long first = -1;
};

// One record from the clean strategy: length, tick, two empty book counts, input count, its two inputs, hash, crc.
constexpr size_t kCleanRecordBytes = 4 + sizeof(fr::Tick) + 2 * 4 + 4 + 2 * 8 + 8 + 4;

std::string g_tool;
fs::path g_dir;
int g_log_id = 0;
std::string g_failure;

fs::path new_log() { return g_dir / ("log_" + std::to_string(g_log_id++)); }

Result tool(const std::string& args) {
    const std::string command = g_tool + " " + args + " 2>/dev/null";
    Result result;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return result;
    char buffer[256];
    while (std::fgets(buffer, sizeof buffer, pipe)) result.out += buffer;
    const int status = pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return result;
}

Result record(const std::string& strategy, const fs::path& log, uint64_t events, uint64_t at = 0,
              const std::string& extra = "") {
    return tool("record " + strategy + " " + log.string() + " " + std::to_string(events) + " " +
                std::to_string(at) + " " + extra);
}

Replay replay(const std::string& strategy, const fs::path& log, uint64_t at = 0) {
    Result raw = tool("replay " + strategy + " " + log.string() + " " + std::to_string(at));
    Replay parsed;
    parsed.exit_code = raw.exit_code;
    std::istringstream words(raw.out);
    std::string word;
    while (words >> word) {
        const auto eq = word.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
        if (key == "status") parsed.status = value;
        if (key == "events") parsed.events = std::stoll(value);
        if (key == "first") parsed.first = std::stoll(value);
    }
    return parsed;
}

void expect(bool ok, const std::string& message) {
    if (!ok && g_failure.empty()) g_failure = message;
}

std::string show(const Replay& r) {
    return "exit=" + std::to_string(r.exit_code) + " status=" + r.status + " events=" +
           std::to_string(r.events) + " first=" + std::to_string(r.first);
}

void expect_match(const Replay& r, long long events) {
    expect(r.exit_code == 0 && r.status == "match" && r.events == events && r.first == -1,
           "expected a full match of " + std::to_string(events) + " events, got " + show(r));
}

void expect_recorded(const Result& r) {
    expect(r.exit_code == 0, "record failed with exit " + std::to_string(r.exit_code));
}

void expect_caught(const std::string& strategy, uint64_t events, uint64_t at) {
    const fs::path log = new_log();
    expect_recorded(record(strategy, log, events, at));
    const Replay r = replay(strategy, log, at);
    expect(r.exit_code == 0 && r.status == "diverged" && r.first == static_cast<long long>(at) &&
               r.events == static_cast<long long>(at),
           strategy + " leak at " + std::to_string(at) + " not pinned exactly, got " + show(r));
}

fs::path copy_prefix(const fs::path& log, uintmax_t bytes) {
    const fs::path cut = new_log();
    fs::copy_file(log, cut);
    fs::resize_file(cut, bytes);
    return cut;
}

void clean_replay_matches() {
    const fs::path log = new_log();
    expect_recorded(record("clean", log, 100000));
    expect_match(replay("clean", log), 100000);
}

void replay_is_repeatable() {
    const fs::path log = new_log();
    expect_recorded(record("clean", log, 5000));
    expect_match(replay("clean", log), 5000);
    expect_match(replay("clean", log), 5000);
}

// 56 MB through a 16 MB ring: records wrap past the end of it several times.
void large_run_wraps_the_ring() {
    const fs::path log = new_log();
    expect_recorded(record("clean", log, 1000000));
    expect_match(replay("clean", log), 1000000);
}

std::string day_line(const std::string& out) {
    const auto at = out.find("ravi ");
    return at == std::string::npos ? "" : out.substr(at);
}

void real_ravi_replays_to_the_same_cent() {
    const fs::path log = new_log();
    const Result recorded = record("ravi", log, 200000);
    expect_recorded(recorded);
    expect(recorded.out.find("fills=0 ") == std::string::npos, "Ravi never traded, so this proves little");
    const Result replayed = tool("replay ravi " + log.string() + " 0");
    expect(replayed.out.find("status=match events=200000") != std::string::npos, "replay: " + replayed.out);
    expect(!day_line(recorded.out).empty() && day_line(recorded.out) == day_line(replayed.out),
           "day differs: " + recorded.out + " vs " + replayed.out);
}

// The number after "name=" in a tool's output, or -2 if it isn't there.
long long field(const std::string& out, const std::string& name) {
    const auto at = out.find(name + "=");
    return at == std::string::npos ? -2 : std::atoll(out.c_str() + at + name.size() + 1);
}

std::string fork_status(const std::string& out) {
    const auto at = out.find("fork status=");
    return at == std::string::npos ? "" : out.substr(at + 12, out.find(' ', at + 12) - at - 12);
}

std::string line_starting(const std::string& out, const std::string& start) {
    const auto at = out.find(start);
    return at == std::string::npos ? "" : out.substr(at + start.size(), out.find('\n', at) - at - start.size());
}

void fork_without_a_change_never_differs() {
    const fs::path log = new_log();
    expect_recorded(record("ravi", log, 200000));
    const Result r = tool("fork ravi " + log.string() + " 100000 0");
    expect(fork_status(r.out) == "done" && field(r.out, "different") == 0 && field(r.out, "first_difference") == -1,
           "an unchanged fork differed: " + r.out);
    expect(!line_starting(r.out, "original ").empty() &&
               line_starting(r.out, "original ") == line_starting(r.out, "forked "),
           "an unchanged fork ended differently: " + r.out);
}

void fork_changes_nothing_before_its_point() {
    const fs::path log = new_log();
    expect_recorded(record("ravi", log, 200000));
    const Result r = tool("fork ravi " + log.string() + " 100000 0 lean_cents=0");
    expect(fork_status(r.out) == "done" && field(r.out, "first_difference") == 100000 && field(r.out, "different") > 0,
           "fork with no lean: " + r.out);
    const Result again = tool("fork ravi " + log.string() + " 100000 0 lean_cents=0");
    expect(again.out == r.out, "the same fork gave two answers: " + r.out + " vs " + again.out);
}

void fork_past_the_end_never_forks() {
    const fs::path log = new_log();
    expect_recorded(record("ravi", log, 1000));
    const Result r = tool("fork ravi " + log.string() + " 5000 0 lean_cents=0");
    expect(fork_status(r.out) == "never_forked" && field(r.out, "events") == 1000, "fork past the end: " + r.out);
}

void fork_on_a_cut_log_uses_the_verified_part() {
    const fs::path log = new_log();
    expect_recorded(record("ravi", log, 10000));
    fs::resize_file(log, fs::file_size(log) - 40);  // loses the footer and part of the last record
    const Result r = tool("fork ravi " + log.string() + " 5000 0 lean_cents=0");
    expect(fork_status(r.out) == "torn" && field(r.out, "events") == 9999, "fork on a cut log: " + r.out);
}

void fork_refuses_a_leaky_original() {
    const fs::path log = new_log();
    expect_recorded(record("wall_clock", log, 10000, 5000));
    for (const char* fork_at : {"9000", "2000"}) {  // the leak before the fork, then after it
        const Result r = tool("fork wall_clock " + log.string() + " " + fork_at + " 5000");
        expect(fork_status(r.out) == "diverged" && field(r.out, "first") == 5000,
               std::string("leaky original, fork at ") + fork_at + ": " + r.out);
    }
}

// The fork reads an extra value at 3000, before its change at 5000, so it isn't the
// same strategy as the recording and must be refused.
void fork_must_match_before_its_point() {
    const fs::path log = new_log();
    expect_recorded(record("extra_read", log, 10000, 3000));
    const Result r = tool("fork extra_read " + log.string() + " 5000 3000");
    expect(fork_status(r.out) == "diverged" && field(r.out, "first") == 3000, "fork differed before its point: " + r.out);
}

void fork_that_needs_more_inputs_fails_loudly() {
    const fs::path log = new_log();
    expect_recorded(record("extra_read", log, 10000, 7000));
    const Result r = tool("fork extra_read " + log.string() + " 5000 7000");
    expect(fork_status(r.out) == "needs_more_inputs" && field(r.out, "needs_input_at") == 7000,
           "fork asking for more inputs: " + r.out);
}

void fork_with_an_unknown_setting_fails() {
    const fs::path log = new_log();
    expect_recorded(record("ravi", log, 1000));
    const Result r = tool("fork ravi " + log.string() + " 500 0 lean=300");
    expect(r.exit_code == 2 && r.out.find("fork status") == std::string::npos, "typo in a setting: " + r.out);
}

void empty_run_matches() {
    const fs::path log = new_log();
    expect_recorded(record("clean", log, 0));
    expect_match(replay("clean", log), 0);
}

void wall_clock_leak_is_pinned() {
    for (uint64_t at : {0, 7777, 9999}) expect_caught("wall_clock", 10000, at);
}

void random_leak_is_pinned() {
    for (uint64_t at : {0, 7777, 9999}) expect_caught("random", 10000, at);
}

void iteration_leak_is_pinned() {
    for (uint64_t at : {0, 7777, 9999}) expect_caught("iteration", 10000, at);
}

void extra_gate_read_is_pinned() {
    for (uint64_t at : {0, 7777, 9999}) expect_caught("extra_read", 10000, at);
}

void skipped_gate_read_is_pinned() {
    for (uint64_t at : {0, 7777, 9999}) expect_caught("skipped_read", 10000, at);
}

void cut_log_is_torn_at_every_sampled_offset() {
    const uint64_t events = 2000;
    const fs::path log = new_log();
    expect_recorded(record("clean", log, events));
    const uintmax_t size = fs::file_size(log);
    expect(size > 64, "log is implausibly small");

    std::set<uintmax_t> offsets = {0, 1, 2, 3, size / 2, size - 2, size - 1};
    for (uintmax_t i = 1; i < 200; ++i) offsets.insert(size * i / 200);
    for (uintmax_t i = 1; i <= 64 && i < size; ++i) offsets.insert(size - i);

    long long previous = 0;
    for (uintmax_t offset : offsets) {
        const fs::path cut = copy_prefix(log, offset);
        const Replay r = replay("clean", cut);
        const std::string where = "cut at byte " + std::to_string(offset) + " of " + std::to_string(size);
        expect(r.exit_code == 0 && r.status == "torn" && r.first == -1, where + ", got " + show(r));
        expect(r.events >= previous && r.events <= static_cast<long long>(events),
               where + " recovered a wrong count, got " + show(r));
        if (offset == size / 2) expect(r.events > 0, where + " recovered nothing, got " + show(r));
        previous = std::max(previous, r.events);
        fs::remove(cut);
    }
}

void crash_mid_record_leaves_a_replayable_prefix() {
    const uint64_t at = 50000;
    const fs::path log = new_log();
    const Result crashed = record("crash", log, 100000, at);
    expect(crashed.exit_code == 42, "crash strategy did not crash, exit " + std::to_string(crashed.exit_code));
    expect(fs::exists(log), "no log after a crash");
    const Replay r = replay("clean", log);
    expect(r.exit_code == 0 && r.status == "torn" && r.first == -1 && r.events >= 0 &&
               r.events <= static_cast<long long>(at),
           "after a crash at " + std::to_string(at) + ", got " + show(r));
    std::printf("      info: crash at event %llu kept %lld events\n", static_cast<unsigned long long>(at), r.events);
}

void expect_damage_reported(const Replay& r, long long most_events, const std::string& where) {
    expect(r.exit_code == 0 && (r.status == "corrupt" || r.status == "torn") && r.first == -1 &&
               r.events >= 0 && r.events <= most_events,
           where + ", got " + show(r));
}

void flipped_byte_is_never_trusted() {
    const uint64_t events = 2000;
    const uintmax_t header = 8, record_bytes = kCleanRecordBytes, footer = 16;
    const fs::path log = new_log();
    expect_recorded(record("clean", log, events));
    const uintmax_t size = fs::file_size(log);
    expect(size == header + events * record_bytes + footer, "unexpected log size " + std::to_string(size));

    std::set<uintmax_t> offsets = {0, 7, 8, 9, size - 5, size - 4, size - 1};
    for (uintmax_t i = 0; i < 67; ++i) offsets.insert(header + i * 3);  // every field of the first records
    for (uintmax_t i = 1; i < 300; ++i) offsets.insert(size * i / 300);

    for (uintmax_t offset : offsets) {
        const fs::path flipped = new_log();
        fs::copy_file(log, flipped);
        std::fstream file(flipped, std::ios::in | std::ios::out | std::ios::binary);
        file.seekg(static_cast<std::streamoff>(offset));
        const char byte = static_cast<char>(file.get());
        file.seekp(static_cast<std::streamoff>(offset));
        file.put(static_cast<char>(byte ^ 0x40));
        file.close();

        const long long damaged_record =
            offset < header ? 0 : static_cast<long long>(std::min<uintmax_t>((offset - header) / record_bytes, events));
        expect_damage_reported(replay("clean", flipped), damaged_record,
                               "flip at byte " + std::to_string(offset));
        fs::remove(flipped);
    }
}

void rewrite_log(const fs::path& log, const std::function<void(std::string&)>& change) {
    std::ifstream in(log, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    change(bytes);
    std::ofstream(log, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void expect_corrupt(const std::function<void(std::string&)>& change, long long events, const std::string& what) {
    const fs::path log = new_log();
    expect_recorded(record("clean", log, 100));
    rewrite_log(log, change);
    const Replay r = replay("clean", log);
    expect(r.exit_code == 0 && r.status == "corrupt" && r.events == events && r.first == -1, what + ", got " + show(r));
}

void bytes_after_the_footer_are_corrupt() {
    expect_corrupt([](std::string& b) { b.push_back('Z'); }, 100, "byte after the footer");
}

void damaged_length_mid_log_is_corrupt() {
    expect_corrupt([](std::string& b) { b[8 + kCleanRecordBytes * 10 + 3] ^= 0x40; }, 10, "huge length at record 10");
}

void removed_record_is_corrupt() {
    expect_corrupt([](std::string& b) { b.erase(8 + kCleanRecordBytes * 5, kCleanRecordBytes); }, 5, "record 5 removed");
}

void removed_last_record_is_corrupt() {
    expect_corrupt([](std::string& b) { b.erase(8 + kCleanRecordBytes * 99, kCleanRecordBytes); }, 99, "last record removed");
}

void zeroed_tail_is_reported() {  // a power cut can leave blocks of zeros, not a clean cut
    const uint64_t events = 2000;
    const fs::path log = new_log();
    expect_recorded(record("clean", log, events));
    const uintmax_t size = fs::file_size(log);
    {
        std::fstream file(log, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(size / 2));
        const std::string zeros(size - size / 2, '\0');
        file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    const Replay r = replay("clean", log);
    expect_damage_reported(r, events / 2, "zeroed second half");
    expect(r.events > 0, "recovered nothing before the zeros, got " + show(r));
}

void kill_at_random_moments_leaves_a_replayable_prefix() {
    long long kept = 0;
    for (int round = 0; round < 8; ++round) {
        const fs::path log = new_log();
        const std::string wait_ms = std::to_string(30 + round * 25);
        const std::string command = g_tool + " record clean " + log.string() +
                                    " 50000000 0 >/dev/null 2>&1 & p=$!; sleep 0." + (wait_ms.size() == 2 ? "0" : "") +
                                    wait_ms + "; kill -9 $p; wait $p 2>/dev/null";
        std::system(command.c_str());
        const Replay r = replay("clean", log);
        expect(r.exit_code == 0 && r.status == "torn" && r.first == -1,
               "after kill -9 at " + wait_ms + " ms, got " + show(r));
        kept += r.events;
        fs::remove(log);
    }
    std::printf("      info: 8 kills kept %lld events in total\n", kept);
}

void full_disk_fails_loudly() {
    const fs::path log = new_log();
    const Result full = record("clean", log, 1000000, 0, "65536");
    expect(full.exit_code == 2, "record claimed success on a full disk, exit " + std::to_string(full.exit_code));
    if (fs::exists(log)) {
        const Replay r = replay("clean", log);
        expect(r.exit_code == 0 && r.status == "torn" && r.first == -1,
               "partial log after a full disk, got " + show(r));
    }
}

void existing_recording_is_never_overwritten() {
    const fs::path log = new_log();
    expect_recorded(record("clean", log, 100));
    const uintmax_t size = fs::file_size(log);
    const Result second = record("clean", log, 50);
    expect(second.exit_code == 2, "second record over a log, exit " + std::to_string(second.exit_code));
    expect(fs::file_size(log) == size, "existing log changed size");
    expect_match(replay("clean", log), 100);
}

// feed is a shell command whose output is the streamed feed.
Result record_feed(const std::string& feed, const std::string& strategy, const fs::path& log) {
    const std::string command = "{ " + feed + "; } | " + g_tool + " record-stdin " + strategy + " " + log.string() + " 2>/dev/null";
    Result result;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return result;
    char buffer[256];
    while (std::fgets(buffer, sizeof buffer, pipe)) result.out += buffer;
    const int status = pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return result;
}

Result record_lines(const std::string& lines, const fs::path& log) {
    return record_feed("printf '" + lines + "'", "clean", log);
}

// A sub-second run can't tell the gate's clock from a stopwatch, so this feed
// pauses for over a second twice. Replay doesn't pause, so a strategy that
// times itself must diverge, while real Ravi must still match.
void stopwatch_leak_is_caught_once_seconds_pass() {
    std::string chunk[3];
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 50; ++i) {
            const long long bid = 7'800'000'000'000LL + (c * 50 + i) % 7 * 1'000'000;  // the mid wanders by cents
            chunk[c] += "0 0 0 " + std::to_string(bid) + " " + std::to_string(bid + 1'000'000) + "\\n";
        }
    }
    const std::string feed = "printf '" + chunk[0] + "'; sleep 1.2; printf '" + chunk[1] + "'; sleep 1.2; printf '" +
                             chunk[2] + "end\\n'";
    for (const std::string strategy : {"ravi", "ravi_stopwatch"}) {
        const fs::path log = new_log();
        expect_recorded(record_feed(feed, strategy, log));
        const Replay r = replay(strategy, log);
        if (strategy == "ravi") {
            expect(r.status == "match" && r.events == 150, "real Ravi over a paced feed, got " + show(r));
        } else {
            expect(r.status == "diverged", "stopwatch leak not caught after a pause, got " + show(r));
        }
    }
}

// tests/fixtures/v1_ravi.log was recorded before queue sizes existed (FRLOG01, 48-byte ticks).
void old_format_log_still_replays() {
    for (const char* fixture : {"tests/fixtures/v1_ravi.log", "tests/fixtures/v2_ravi.log"}) {  // FRLOG01, FRLOG02
        const Result r = tool(std::string("replay ravi ") + fixture + " 0");
        expect(r.out.find("status=match events=2000") != std::string::npos, std::string(fixture) + ": " + r.out);
        expect(r.out.find("ravi fills=1 lots=1 most_lots=1 gross_usd=0.00023 fees_usd=0.07800") != std::string::npos,
               std::string(fixture) + " day changed: " + r.out);
    }
}

void queue_sizes_are_recorded_and_replayed() {
    const fs::path log = new_log();
    expect_recorded(record_feed("printf '100 5 1 99 101 900 100\\n0 0 0 99 101 40 60\\nend\\n'", "clean", log));
    expect_match(replay("clean", log), 2);
    std::ifstream in(log, std::ios::binary);
    char magic[8] = {};
    in.read(magic, sizeof magic);
    expect(std::string(magic) == "FRLOG03", "new logs must say FRLOG03, got " + std::string(magic));
}

// A book line, a trade with the exchange's time, then a smaller book: the strategy
// folds all of it in, so anything lost or garbled in the log diverges.
void books_and_exchange_times_replay_exactly() {
    const fs::path log = new_log();
    const std::string lines =
        "book 3 2 99 5 98 7 97 9 101 4 102 6\\n"
        "100 5 1 99 101 5 4 1794542361972000000\\n"
        "book 1 1 99 1 101 1\\n"
        "0 0 0 99 101 1 1\\nend\\n";
    expect_recorded(record_feed("printf '" + lines + "'", "book_clean", log));
    expect_match(replay("book_clean", log), 4);
    const Result fork = tool("fork book_clean " + log.string() + " 2 0");
    expect(fork_status(fork.out) == "done" && field(fork.out, "different") == 0, "fork over books: " + fork.out);
}

// What goes in must come out: record feed lines, dump them back, and compare.
void dump_gives_back_exactly_what_was_fed() {
    const fs::path log = new_log();
    const std::string fed =
        "book 3 2 99 5 98 7 97 9 101 4 102 6\n"
        "100 5 1 99 101 5 4 1794542361972000000\n"
        "book 0 1 101 1\n"
        "0 0 0 99 101 1 1 0\n";
    std::string escaped;
    for (char c : fed) escaped += c == '\n' ? std::string("\\n") : std::string(1, c);
    expect_recorded(record_feed("printf '" + escaped + "end\\n'", "book_clean", log));
    const Result dumped = tool("dump book_clean " + log.string());
    expect(dumped.exit_code == 0 && dumped.out == fed, "dump differs from the feed:\n" + dumped.out);
}

// Worked by hand. Sizes are in units of 0.01 BTC (1,000,000 raw); prices in raw units.
//   book: bid 100 x 2.00, ask 101 x 3.00   -> probes A (bid, 2.00 ahead) and B (ask, 3.00 ahead)
//   a seller trades 1.00 at 100             -> A touched; 1.00 still ahead of A
//   book: bid 100 x 0.50, ask 101 x 3.00   -> 2.00 - 1.00 traded - 0.50 left = 0.50 cancelled;
//                                              A's share is 1.00/2.00, so 0.75 ahead in the queue
//                                              model. Probes C (bid, 0.50) and D (ask, 3.00) start.
//   a seller trades 0.80 at 100             -> A: queue fill (0.75 used up), not certain (0.20 left).
//                                              C: certain and queue fill (0.50 used up)
//   a buyer trades at 102                   -> B and D: through, so filled by every rule
void fill_rules_match_a_hand_worked_case() {
    const fs::path log = new_log();
    const std::string feed =
        "book 1 1 100 200000000 101 300000000\\n"
        "100 100000000 -1 100 101 0 0 0\\n"
        "book 1 1 100 50000000 101 300000000\\n"
        "100 80000000 -1 100 101 0 0 0\\n"
        "102 10000000 1 100 101 0 0 0\\nend\\n";
    expect_recorded(record_feed("printf '" + feed + "'", "ravi", log));
    const fs::path csv = g_dir / "fill_hand.csv";
    const std::string study = (fs::path(g_tool).parent_path() / "fill_study").string();
    expect(std::system((study + " --flush " + csv.string() + " " + log.string() + " >/dev/null").c_str()) == 0,
           "fill_study failed");
    std::ifstream in(csv);
    std::string line, got;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        // side, then the 1-second answer of each rule: touched, certain, strict, queue.
        std::vector<std::string> cells;
        std::string cell;
        for (char c : line + ",") {
            if (c == ',') { cells.push_back(cell); cell.clear(); } else { cell += c; }
        }
        got += cells[2] + ":" + cells[16] + cells[19] + cells[22] + cells[25] + " ";
    }
    expect(got == "1:1001 -1:1111 1:1101 -1:1111 ", "fill rules per probe (side:touched certain strict queue): " + got);
}

void bad_book_lines_are_refused() {
    for (const std::string line : {"book 2 1 99 5 101 4", "book 1 1 99 5 101", "book -1 0", "book 1 0 99 x"}) {
        const fs::path log = new_log();
        const Result r = record_feed("printf '" + line + "\\nend\\n'", "book_clean", log);
        expect(r.exit_code == 2, "book line \"" + line + "\" must stop the run, exit " + std::to_string(r.exit_code));
    }
}

void oversized_book_is_refused() {
    std::string line = "book 1001 0";
    for (int i = 0; i < 1001; ++i) line += " 99 1";
    const fs::path log = new_log();
    const Result r = record_feed("printf '" + line + "\\nend\\n'", "book_clean", log);
    expect(r.exit_code == 2, "a 1001-level book side must be refused, exit " + std::to_string(r.exit_code));
}

void six_field_feed_line_is_refused() {
    const fs::path log = new_log();
    const Result r = record_lines("100 5 1 99 101 900\\nend\\n", log);
    expect(r.exit_code == 2, "a line with only one queue size must stop the run, exit " + std::to_string(r.exit_code));
}

void streamed_feed_replays_from_the_log_alone() {
    const fs::path log = new_log();
    expect_recorded(record_lines("100 5 1 99 101\\n101 7 -1 100 102\\n0 0 0 99 100\\nend\\n", log));
    expect_match(replay("clean", log), 3);
}

void dead_feed_never_looks_finished() {
    const fs::path log = new_log();
    const Result dead = record_lines("100 5 1 99 101\\n101 7 -1 100 102\\n", log);
    expect(dead.exit_code == 2, "feed died without its end line, exit " + std::to_string(dead.exit_code));
    const Replay r = replay("clean", log);
    expect(r.exit_code == 0 && r.status == "torn" && r.events == 2, "after a dead feed, got " + show(r));
}

void finished_run_is_synced_to_disk() {
    const fs::path log = new_log();
    const Result r = record_lines("100 5 1 99 101\\n101 7 -1 100 102\\nend\\n", log);
    expect_recorded(r);
    expect(r.out.find("disk_syncs=0") == std::string::npos, "run finished without a disk sync: " + r.out);
}

void bad_feed_line_stops_the_run_and_keeps_the_prefix() {
    const fs::path log = new_log();
    const Result bad = record_lines("100 5 1 99 101\\n101 7 -1 100 102\\nnot a tick\\n99 2 1 98 100\\n", log);
    expect(bad.exit_code == 2, "bad feed line, exit " + std::to_string(bad.exit_code));
    const Replay r = replay("clean", log);
    expect(r.exit_code == 0 && r.status == "torn" && r.first == -1 && r.events == 2,
           "after a bad feed line, got " + show(r));
}

void unwritable_path_fails() {
    const Result r = record("clean", g_dir / "no_such_dir" / "log", 10);
    expect(r.exit_code == 2, "record to a bad path, exit " + std::to_string(r.exit_code));
}

void folder_as_log_fails() {
    const Replay r = replay("clean", g_dir);
    expect(r.exit_code == 2, "replay of a folder, got " + show(r));
}

void failed_setup_leaves_no_file() {
    const fs::path dir = g_dir / "write_only";
    fs::create_directory(dir);
    fs::permissions(dir, fs::perms::owner_write | fs::perms::owner_exec);  // can create a file, can't sync the folder
    const fs::path log = dir / "log";
    const Result r = record("clean", log, 10);
    fs::permissions(dir, fs::perms::owner_all);
    expect(r.exit_code == 2, "folder can't be synced, exit " + std::to_string(r.exit_code));
    expect(!fs::exists(log), "failed setup left a file behind");
}

void missing_log_fails() {
    const Replay r = replay("clean", g_dir / "no_such_log");
    expect(r.exit_code == 2, "replay of a missing log, got " + show(r));
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path build = argc > 1 ? argv[1] : "build";
    g_tool = (build / "fr_tool").string();
    g_dir = build / "test_logs";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"clean_replay_matches", clean_replay_matches},
        {"replay_is_repeatable", replay_is_repeatable},
        {"large_run_wraps_the_ring", large_run_wraps_the_ring},
        {"real_ravi_replays_to_the_same_cent", real_ravi_replays_to_the_same_cent},
        {"stopwatch_leak_is_caught_once_seconds_pass", stopwatch_leak_is_caught_once_seconds_pass},
        {"fork_without_a_change_never_differs", fork_without_a_change_never_differs},
        {"fork_changes_nothing_before_its_point", fork_changes_nothing_before_its_point},
        {"fork_past_the_end_never_forks", fork_past_the_end_never_forks},
        {"fork_on_a_cut_log_uses_the_verified_part", fork_on_a_cut_log_uses_the_verified_part},
        {"fork_refuses_a_leaky_original", fork_refuses_a_leaky_original},
        {"fork_must_match_before_its_point", fork_must_match_before_its_point},
        {"fork_that_needs_more_inputs_fails_loudly", fork_that_needs_more_inputs_fails_loudly},
        {"fork_with_an_unknown_setting_fails", fork_with_an_unknown_setting_fails},
        {"old_format_log_still_replays", old_format_log_still_replays},
        {"queue_sizes_are_recorded_and_replayed", queue_sizes_are_recorded_and_replayed},
        {"six_field_feed_line_is_refused", six_field_feed_line_is_refused},
        {"books_and_exchange_times_replay_exactly", books_and_exchange_times_replay_exactly},
        {"dump_gives_back_exactly_what_was_fed", dump_gives_back_exactly_what_was_fed},
        {"fill_rules_match_a_hand_worked_case", fill_rules_match_a_hand_worked_case},
        {"bad_book_lines_are_refused", bad_book_lines_are_refused},
        {"oversized_book_is_refused", oversized_book_is_refused},
        {"empty_run_matches", empty_run_matches},
        {"wall_clock_leak_is_pinned", wall_clock_leak_is_pinned},
        {"random_leak_is_pinned", random_leak_is_pinned},
        {"iteration_leak_is_pinned", iteration_leak_is_pinned},
        {"extra_gate_read_is_pinned", extra_gate_read_is_pinned},
        {"skipped_gate_read_is_pinned", skipped_gate_read_is_pinned},
        {"cut_log_is_torn_at_every_sampled_offset", cut_log_is_torn_at_every_sampled_offset},
        {"crash_mid_record_leaves_a_replayable_prefix", crash_mid_record_leaves_a_replayable_prefix},
        {"flipped_byte_is_never_trusted", flipped_byte_is_never_trusted},
        {"bytes_after_the_footer_are_corrupt", bytes_after_the_footer_are_corrupt},
        {"damaged_length_mid_log_is_corrupt", damaged_length_mid_log_is_corrupt},
        {"removed_record_is_corrupt", removed_record_is_corrupt},
        {"removed_last_record_is_corrupt", removed_last_record_is_corrupt},
        {"zeroed_tail_is_reported", zeroed_tail_is_reported},
        {"kill_at_random_moments_leaves_a_replayable_prefix", kill_at_random_moments_leaves_a_replayable_prefix},
        {"full_disk_fails_loudly", full_disk_fails_loudly},
        {"existing_recording_is_never_overwritten", existing_recording_is_never_overwritten},
        {"streamed_feed_replays_from_the_log_alone", streamed_feed_replays_from_the_log_alone},
        {"finished_run_is_synced_to_disk", finished_run_is_synced_to_disk},
        {"dead_feed_never_looks_finished", dead_feed_never_looks_finished},
        {"bad_feed_line_stops_the_run_and_keeps_the_prefix", bad_feed_line_stops_the_run_and_keeps_the_prefix},
        {"unwritable_path_fails", unwritable_path_fails},
        {"folder_as_log_fails", folder_as_log_fails},
        {"failed_setup_leaves_no_file", failed_setup_leaves_no_file},
        {"missing_log_fails", missing_log_fails},
    };

    int failed = 0;
    for (const auto& [name, test] : tests) {
        g_failure.clear();
        try {
            test();
        } catch (const std::exception& error) {
            g_failure = error.what();
        }
        std::printf("%s %s\n", g_failure.empty() ? "PASS" : "FAIL", name.c_str());
        if (!g_failure.empty()) {
            std::printf("      %s\n", g_failure.c_str());
            ++failed;
        }
    }
    std::printf("%zu passed, %d failed\n", tests.size() - failed, failed);
    fs::remove_all(g_dir);
    return failed == 0 ? 0 : 1;
}
