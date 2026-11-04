// The contract between the Flight Recorder core and everything that tests it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fr {

struct Tick {
    uint64_t seq;  // 0, 1, 2, ... in arrival order
    int64_t price;
    int64_t qty;
};

class TickSource {
public:
    virtual ~TickSource() = default;
    virtual bool next(Tick& out) = 0;  // false when the feed ends
};

// The one door. A strategy gets time and randomness only from here.
class Gate {
public:
    virtual ~Gate() = default;
    virtual uint64_t now_ns() = 0;
    virtual uint64_t random_u64() = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual int64_t on_tick(const Tick& tick, Gate& gate) = 0;
    virtual uint64_t state_hash() const = 0;
};

struct RecordReport {
    uint64_t events = 0;
    uint64_t ring_full_waits = 0;  // times the trading thread had to wait for space
    uint64_t disk_syncs = 0;       // times data was forced onto the disk
    uint64_t max_sync_ns = 0;      // the slowest of those
};

enum class ReplayStatus {
    Match,     // the log is complete and every event matched
    Diverged,  // an event differed; first_divergence_seq says which
    TornTail,  // every readable event matched, but the log just stops
    Corrupt,   // every event before the damage matched, then bytes failed their checksum
};

struct ReplayReport {
    ReplayStatus status = ReplayStatus::Match;
    uint64_t events_replayed = 0;  // events that matched
    std::optional<uint64_t> first_divergence_seq;
};

// Runs the strategy on a live feed and writes everything needed to replay it.
// Throws std::runtime_error if log_path already exists, cannot be opened, or
// any write or disk sync fails. An existing recording is never overwritten.
// Returning means the whole log is on the disk. During a run, a process crash
// can lose what is still in the ring, and a power cut can lose about the last
// sync interval. Either way, everything before the loss still replays.
RecordReport record_run(TickSource& source, Strategy& strategy, const std::string& log_path);

// Runs a fresh strategy from the log alone: no feed, real clock or real random.
// An event diverges if its output, its state hash, or the inputs it read from
// the gate differ from the recording. Stops at the first divergence.
// A damaged log is a report, not an exception. Throws std::runtime_error only
// if log_path cannot be opened.
ReplayReport replay_run(Strategy& strategy, const std::string& log_path);

}  // namespace fr
