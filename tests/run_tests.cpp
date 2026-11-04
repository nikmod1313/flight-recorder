// Black-box tests. Each record and replay is a separate fr_tool process.
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
    const uintmax_t header = 8, record_bytes = 60;
    const fs::path log = new_log();
    expect_recorded(record("clean", log, events));
    const uintmax_t size = fs::file_size(log);
    expect(size == header + events * record_bytes + 4, "unexpected log size " + std::to_string(size));

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

Result record_lines(const std::string& lines, const fs::path& log) {
    const std::string command = "printf '" + lines + "' | " + g_tool + " record-stdin clean " + log.string() + " 2>/dev/null";
    Result result;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return result;
    char buffer[256];
    while (std::fgets(buffer, sizeof buffer, pipe)) result.out += buffer;
    const int status = pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return result;
}

void streamed_feed_replays_from_the_log_alone() {
    const fs::path log = new_log();
    expect_recorded(record_lines("100 5\\n101 7\\n99 2\\n", log));
    expect_match(replay("clean", log), 3);
}

void finished_run_is_synced_to_disk() {
    const fs::path log = new_log();
    const Result r = record_lines("100 5\\n101 7\\n", log);
    expect_recorded(r);
    expect(r.out.find("disk_syncs=0") == std::string::npos, "run finished without a disk sync: " + r.out);
}

void bad_feed_line_stops_the_run_and_keeps_the_prefix() {
    const fs::path log = new_log();
    const Result bad = record_lines("100 5\\n101 7\\nnot a tick\\n99 2\\n", log);
    expect(bad.exit_code == 2, "bad feed line, exit " + std::to_string(bad.exit_code));
    const Replay r = replay("clean", log);
    expect(r.exit_code == 0 && r.status == "torn" && r.first == -1 && r.events == 2,
           "after a bad feed line, got " + show(r));
}

void unwritable_path_fails() {
    const Result r = record("clean", g_dir / "no_such_dir" / "log", 10);
    expect(r.exit_code == 2, "record to a bad path, exit " + std::to_string(r.exit_code));
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
        {"empty_run_matches", empty_run_matches},
        {"wall_clock_leak_is_pinned", wall_clock_leak_is_pinned},
        {"random_leak_is_pinned", random_leak_is_pinned},
        {"iteration_leak_is_pinned", iteration_leak_is_pinned},
        {"extra_gate_read_is_pinned", extra_gate_read_is_pinned},
        {"skipped_gate_read_is_pinned", skipped_gate_read_is_pinned},
        {"cut_log_is_torn_at_every_sampled_offset", cut_log_is_torn_at_every_sampled_offset},
        {"crash_mid_record_leaves_a_replayable_prefix", crash_mid_record_leaves_a_replayable_prefix},
        {"flipped_byte_is_never_trusted", flipped_byte_is_never_trusted},
        {"zeroed_tail_is_reported", zeroed_tail_is_reported},
        {"kill_at_random_moments_leaves_a_replayable_prefix", kill_at_random_moments_leaves_a_replayable_prefix},
        {"full_disk_fails_loudly", full_disk_fails_loudly},
        {"existing_recording_is_never_overwritten", existing_recording_is_never_overwritten},
        {"streamed_feed_replays_from_the_log_alone", streamed_feed_replays_from_the_log_alone},
        {"finished_run_is_synced_to_disk", finished_run_is_synced_to_disk},
        {"bad_feed_line_stops_the_run_and_keeps_the_prefix", bad_feed_line_stops_the_run_and_keeps_the_prefix},
        {"unwritable_path_fails", unwritable_path_fails},
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
