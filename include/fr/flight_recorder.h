// The contract between the Flight Recorder core and everything that tests it.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fr {

struct Tick {
    uint64_t seq;  // 0, 1, 2, ... in arrival order
    int64_t price;     // the trade's price; 0 for a book update
    int64_t qty;       // the trade's size; 0 for a book update
    int64_t side;      // who started the trade: +1 buyer, -1 seller, 0 a book update
    int64_t best_bid;  // the highest price anyone will pay right now
    int64_t best_ask;  // the lowest price anyone will accept right now
    int64_t bid_qty;   // how much is waiting at best_bid; 0 if unknown, as in FRLOG01 logs
    int64_t ask_qty;   // how much is waiting at best_ask; 0 if unknown
    int64_t exchange_ns;  // the exchange's own time for a trade, in ns since 1970; 0 if unknown, as before FRLOG03
};

// The best levels of the order book at one moment, best price first.
struct BookLevel {
    int64_t price;
    int64_t qty;
};
struct Book {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

class TickSource {
public:
    virtual ~TickSource() = default;
    virtual bool next(Tick& out) = 0;  // false when the feed ends
    // The book that came with the event next() just gave, or nullptr if none did.
    // Valid until the next call to next().
    virtual const Book* book() const { return nullptr; }
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
    // Called just before on_tick when the event carries a book. Most strategies ignore it.
    virtual void on_book(const Book& book) { (void)book; }
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
    Corrupt,   // every event before the damage matched, then the log failed a check
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

enum class ForkStatus {
    Done,             // the whole log was used, and the fork ran from fork_at to the end
    TornTail,         // as Done, up to where the log was cut off
    NeverForked,      // the log ended before fork_at
    Diverged,         // the original, or the fork before its change, didn't match the recording
    Corrupt,          // the log failed a check
    NeedsMoreInputs,  // after the change, the fork asked the gate for a value the recording doesn't have
};

struct ForkReport {
    ForkStatus status = ForkStatus::Done;
    uint64_t events = 0;                          // events checked against the recording
    std::optional<uint64_t> first_difference_seq;  // first event where the fork decided differently
    uint64_t different_events = 0;                // events where the two decided differently
    std::optional<uint64_t> first_divergence_seq;  // set when status is Diverged
    std::optional<uint64_t> fork_needed_more_inputs_seq;
};

// Runs a fresh strategy from the log alone: no feed, real clock or real random.
// An event diverges if its output, its state hash, or the inputs it read from
// the gate differ from the recording. Stops at the first divergence.
// A damaged log is a report, not an exception. Throws std::runtime_error only
// if log_path cannot be opened or is not a regular file.
ReplayReport replay_run(Strategy& strategy, const std::string& log_path);

// Replays a recorded day with two copies of a strategy. Both must match the
// recording up to event fork_at. Then change(fork) is applied once, and the fork
// carries on with the same recorded ticks and the same recorded gate values,
// in the order it asks for them. The original is still checked to the end.
// Assumes the fork's different decisions don't change the market: the ticks
// after the fork are what really happened. Throws like replay_run.
ForkReport fork_run(Strategy& original, Strategy& fork, uint64_t fork_at,
                    const std::function<void(Strategy&)>& change, const std::string& log_path);

}  // namespace fr
