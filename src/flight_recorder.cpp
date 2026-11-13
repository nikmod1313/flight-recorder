// The Flight Recorder core. The contract is in include/fr/flight_recorder.h.
//
// A log is a header, a run of records, then one sealed footer:
//   [kMagic]
//   [length u32][tick][input count u32][inputs u64 ...][event hash u64][crc u32]
//   [kEndMarker u32][event count u64][crc u32]
// length counts the bytes after itself. crc covers the length and everything
// up to the crc, so damaged bytes are caught instead of being trusted. The
// footer is the only proof that a run finished, so a log without it is
// incomplete even if every record is whole. Its event count catches records
// removed from the end.
//
// FRLOG03 is written now. Its records also carry a book, between the tick and
// the input count: [bid levels u32][ask levels u32][(price, qty) i64 pairs, bids then asks].
// Older logs still replay. FRLOG02 has a 64-byte tick and no book; FRLOG01 a
// 48-byte tick, so its queue sizes read as 0 too.
#include "fr/flight_recorder.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ARM_FEATURE_CRC32) && !defined(FR_SOFT_CRC)
#include <arm_acle.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fr {
namespace {

constexpr uint32_t kEndMarker = 0xFFFFFFFFu;  // no real record is this long
#ifndef FR_RING_BYTES
#define FR_RING_BYTES (size_t{16} << 20)
#endif
constexpr size_t kRingBytes = FR_RING_BYTES;  // must be a power of two
constexpr char kMagic[8] = "FRLOG03";
constexpr char kMagicV2[8] = "FRLOG02";
constexpr char kMagicV1[8] = "FRLOG01";
constexpr size_t kTickBytesV2 = 64;  // the tick before exchange_ns
constexpr size_t kTickBytesV1 = 48;  // the tick before bid_qty and ask_qty
constexpr uint32_t kMaxBookLevels = 1000;  // per side: more is damage, not a book
constexpr auto kSyncInterval = std::chrono::milliseconds(100);
constexpr uint32_t kCrcStart = 0xFFFFFFFFu;
constexpr size_t kFixedRecordBytes = sizeof(Tick) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t);
constexpr size_t kRecordBytesBesideTick = kFixedRecordBytes - sizeof(Tick);
constexpr size_t kBookCountBytes = 2 * sizeof(uint32_t);  // FRLOG03 only
constexpr size_t kFooterBytes = sizeof(uint64_t) + sizeof(uint32_t);  // what follows kEndMarker

[[noreturn]] void fail(const std::string& what, const std::string& path) {
    throw std::runtime_error(what + " " + path + ": " + std::strerror(errno));
}

uint64_t event_hash(int64_t output, uint64_t state) {
    return (static_cast<uint64_t>(output) * 0x9E3779B97F4A7C15ull) ^ state;
}

// CRC-32C. Start from kCrcStart, feed bytes in pieces, then invert the result.
#if defined(__ARM_FEATURE_CRC32) && !defined(FR_SOFT_CRC)
uint32_t crc32c(uint32_t crc, const void* data, size_t bytes) {
    const char* next = static_cast<const char*>(data);
    for (; bytes >= sizeof(uint64_t); bytes -= sizeof(uint64_t), next += sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, next, sizeof word);
        crc = __crc32cd(crc, word);
    }
    for (; bytes > 0; --bytes, ++next) crc = __crc32cb(crc, static_cast<uint8_t>(*next));
    return crc;
}
#else
constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0x82F63B78u : 0);
        table[i] = value;
    }
    return table;
}
constexpr std::array<uint32_t, 256> kCrcTable = make_crc_table();

uint32_t crc32c(uint32_t crc, const void* data, size_t bytes) {
    const unsigned char* next = static_cast<const unsigned char*>(data);
    for (; bytes > 0; --bytes, ++next) crc = kCrcTable[(crc ^ *next) & 0xFF] ^ (crc >> 8);
    return crc;
}
#endif

// Forces written data onto the disk itself. On macOS plain fsync stops at the
// drive's own cache, so a power cut could still lose the data.
bool full_sync(int fd) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
    if (errno != ENOTSUP && errno != EINVAL) return false;  // some file systems lack it
#endif
    return ::fsync(fd) == 0;
}

bool sync_log(int fd) {
#ifdef FR_TEST_SYNC_FAILS
    (void)fd;
    errno = EIO;
    return false;
#else
    return full_sync(fd);
#endif
}

// A new file's name lives in its directory, so that needs saving too.
void sync_parent_directory(const std::string& path) {
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash + 1);
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) fail("cannot open directory of", path);
    const bool synced = full_sync(fd);
    const int error = errno;
    ::close(fd);
    errno = error;
    if (!synced) fail("cannot sync directory of", path);
}

// Two threads share one ring of bytes. The trading thread only copies records
// in. A background thread carries them to the OS, so a slow write() never
// stops the trading thread unless the ring is completely full. Then it waits:
// losing a record would break replay.
class LogWriter {
public:
    explicit LogWriter(const std::string& path) : path_(path), ring_(kRingBytes) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd_ < 0) fail("cannot create log", path_);
        try {
            sync_parent_directory(path_);
            drain_thread_ = std::thread([this] { drain(); });
        } catch (...) {  // no destructor runs here, and an empty log would block a retry
            ::close(fd_);
            ::unlink(path_.c_str());  // safe: O_EXCL means this run created it
            throw;
        }
    }

    ~LogWriter() {
        stop_drain();
        if (fd_ >= 0) ::close(fd_);  // only reached on an error already being thrown
    }

    // Copies bytes into the current record. The drain thread can't see them yet.
    void append(const void* data, size_t bytes) {
        if (staged_ + bytes > ring_.size()) throw std::runtime_error("record is larger than the ring");
        wait_for_space(staged_ + bytes);

        const size_t offset = (head_ + staged_) & (ring_.size() - 1);
        const size_t before_wrap = std::min(bytes, ring_.size() - offset);
        std::memcpy(ring_.data() + offset, data, before_wrap);
        std::memcpy(ring_.data(), static_cast<const char*>(data) + before_wrap, bytes - before_wrap);
        staged_ += bytes;
    }

    // Publishes the whole record to the drain thread in one step.
    void commit() {
        throw_if_drain_failed();  // don't keep trading on a recorder that has died
        head_ += staged_;
        staged_ = 0;
        published_head_.store(head_, std::memory_order_release);
    }

    uint64_t full_waits() const { return full_waits_; }
    uint64_t disk_syncs() const { return disk_syncs_; }  // read these only after finish()
    uint64_t max_sync_ns() const { return max_sync_ns_; }

    void finish() {
        stop_drain();
        throw_if_drain_failed();
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) fail("cannot close log", path_);
    }

private:
    void wait_for_space(size_t needed) {
        bool counted = false;
        while (head_ + needed - cached_tail_ > ring_.size()) {
            throw_if_drain_failed();
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head_ + needed - cached_tail_ <= ring_.size()) return;
            if (!counted) ++full_waits_;
            counted = true;
            std::this_thread::yield();
        }
    }

    void throw_if_drain_failed() {
        const int error = drain_error_.load(std::memory_order_acquire);
        if (error == 0) return;
        errno = error;
        fail("cannot write log", path_);
    }

    void stop_drain() {
        if (!drain_thread_.joinable()) return;
        stop_.store(true, std::memory_order_release);
        drain_thread_.join();
    }

    // Runs on the background thread.
    void drain() {
        uint64_t tail = 0;
        uint64_t synced_to = 0;
        auto last_sync = std::chrono::steady_clock::now();
        for (;;) {
            // Read stop first: anything committed before stop is then seen below.
            const bool stopping = stop_.load(std::memory_order_acquire);
            const uint64_t head = published_head_.load(std::memory_order_acquire);
            const auto now = std::chrono::steady_clock::now();
            if (tail != synced_to && ((stopping && tail == head) || now - last_sync >= kSyncInterval)) {
                if (!timed_sync()) return;
                synced_to = tail;
                last_sync = std::chrono::steady_clock::now();
            }
            if (tail == head) {
                if (stopping) return;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            const size_t offset = tail & (ring_.size() - 1);
            const size_t bytes = std::min<uint64_t>(head - tail, ring_.size() - offset);
            if (!write_all(ring_.data() + offset, bytes)) {
                drain_error_.store(errno, std::memory_order_release);
                return;
            }
            tail += bytes;
            tail_.store(tail, std::memory_order_release);
        }
    }

    bool timed_sync() {
        const auto start = std::chrono::steady_clock::now();
        if (!sync_log(fd_)) {  // never retried: a failed sync may have dropped the data
            drain_error_.store(errno, std::memory_order_release);
            return false;
        }
        const auto taken = std::chrono::steady_clock::now() - start;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(taken).count();
        max_sync_ns_ = std::max(max_sync_ns_, static_cast<uint64_t>(ns));
        ++disk_syncs_;
        return true;
    }

    bool write_all(const char* next, size_t bytes) {
        while (bytes > 0) {
            const ssize_t written = ::write(fd_, next, bytes);
            if (written < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            next += written;
            bytes -= static_cast<size_t>(written);
        }
        return true;
    }

    std::string path_;
    int fd_ = -1;
    std::vector<char> ring_;  // zero-filled here, so no page faults during the run
    std::thread drain_thread_;

    // Only the trading thread touches these.
    uint64_t head_ = 0;
    uint64_t cached_tail_ = 0;
    size_t staged_ = 0;
    uint64_t full_waits_ = 0;

    // Only the drain thread touches these until it has been joined.
    uint64_t disk_syncs_ = 0;
    uint64_t max_sync_ns_ = 0;

    // Shared. Each sits on its own cache line so the threads don't slow each other.
    alignas(64) std::atomic<uint64_t> published_head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<bool> stop_{false};
    std::atomic<int> drain_error_{0};
};

// The door while recording: real values go out and each one is kept.
class RecordingGate final : public Gate {
public:
    RecordingGate() : rng_(std::random_device{}()) { inputs_.reserve(16); }

    uint64_t now_ns() override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return keep(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
    }
    uint64_t random_u64() override { return keep(rng_()); }

    const std::vector<uint64_t>& inputs() const { return inputs_; }
    void next_event() { inputs_.clear(); }

private:
    uint64_t keep(uint64_t value) {
        inputs_.push_back(value);
        return value;
    }

    std::mt19937_64 rng_;
    std::vector<uint64_t> inputs_;
};

// The door while replaying: hands back recorded values in order. Reading past
// the end returns 0 and is remembered, so the event is reported, not crashed.
class ReplayGate final : public Gate {
public:
    uint64_t now_ns() override { return take(); }
    uint64_t random_u64() override { return take(); }

    void next_event(const char* inputs, uint32_t count) {
        inputs_ = inputs;
        count_ = count;
        cursor_ = 0;
        overran_ = false;
    }
    bool read_exactly_all() const { return !overran_ && cursor_ == count_; }
    bool overran() const { return overran_; }

private:
    uint64_t take() {
        if (cursor_ == count_) {
            overran_ = true;
            return 0;
        }
        uint64_t value;
        std::memcpy(&value, inputs_ + cursor_ * sizeof value, sizeof value);
        ++cursor_;
        return value;
    }

    const char* inputs_ = nullptr;
    uint32_t count_ = 0;
    uint32_t cursor_ = 0;
    bool overran_ = false;
};

// The log mapped straight into memory. The OS pages it in as replay walks it,
// so a log can be larger than RAM.
class MappedLog {
public:
    explicit MappedLog(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("cannot open log", path);
        struct stat info;
        const bool have_info = ::fstat(fd, &info) == 0;
        if (!have_info || !S_ISREG(info.st_mode)) {
            const int error = have_info ? EINVAL : errno;
            ::close(fd);
            errno = error;
            fail("not a log file:", path);
        }
        size_ = static_cast<size_t>(info.st_size);
        if (size_ > 0) {
            void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            const int error = errno;
            ::close(fd);
            errno = error;
            if (mapped == MAP_FAILED) fail("cannot map log", path);
            data_ = static_cast<const char*>(mapped);
        } else {
            ::close(fd);
        }
    }
    ~MappedLog() {
        if (data_) ::munmap(const_cast<char*>(data_), size_);
    }
    MappedLog(const MappedLog&) = delete;
    MappedLog& operator=(const MappedLog&) = delete;

    const char* begin() const { return data_; }
    const char* end() const { return data_ + size_; }

private:
    const char* data_ = nullptr;
    size_t size_ = 0;
};

template <typename T>
T read_at(const char* from) {
    T value;
    std::memcpy(&value, from, sizeof value);
    return value;
}

}  // namespace

RecordReport record_run(TickSource& source, Strategy& strategy, const std::string& log_path) {
    LogWriter log(log_path);
    RecordingGate gate;
    RecordReport report;

    log.append(kMagic, sizeof kMagic);
    log.commit();

    Tick tick;
    const Book no_book;
    while (source.next(tick)) {
        const Book* book = source.book();
        if (book && (book->bids.size() > kMaxBookLevels || book->asks.size() > kMaxBookLevels)) {
            throw std::runtime_error("book has more than 1000 levels on a side");
        }
        if (!book) book = &no_book;
        gate.next_event();
        if (!book->bids.empty() || !book->asks.empty()) strategy.on_book(*book);
        const int64_t output = strategy.on_tick(tick, gate);
        const uint64_t hash = event_hash(output, strategy.state_hash());

        const auto& inputs = gate.inputs();
        const uint32_t count = static_cast<uint32_t>(inputs.size());
        const size_t input_bytes = inputs.size() * sizeof(uint64_t);
        const uint32_t bid_levels = static_cast<uint32_t>(book->bids.size());
        const uint32_t ask_levels = static_cast<uint32_t>(book->asks.size());
        const size_t book_bytes = (book->bids.size() + book->asks.size()) * sizeof(BookLevel);
        const uint32_t length = static_cast<uint32_t>(kFixedRecordBytes + kBookCountBytes + book_bytes + input_bytes);

        uint32_t crc = crc32c(kCrcStart, &length, sizeof length);
        crc = crc32c(crc, &tick, sizeof tick);
        crc = crc32c(crc, &bid_levels, sizeof bid_levels);
        crc = crc32c(crc, &ask_levels, sizeof ask_levels);
        crc = crc32c(crc, book->bids.data(), book->bids.size() * sizeof(BookLevel));
        crc = crc32c(crc, book->asks.data(), book->asks.size() * sizeof(BookLevel));
        crc = crc32c(crc, &count, sizeof count);
        crc = crc32c(crc, inputs.data(), input_bytes);
        crc = ~crc32c(crc, &hash, sizeof hash);

        log.append(&length, sizeof length);
        log.append(&tick, sizeof tick);
        log.append(&bid_levels, sizeof bid_levels);
        log.append(&ask_levels, sizeof ask_levels);
        log.append(book->bids.data(), book->bids.size() * sizeof(BookLevel));
        log.append(book->asks.data(), book->asks.size() * sizeof(BookLevel));
        log.append(&count, sizeof count);
        log.append(inputs.data(), input_bytes);
        log.append(&hash, sizeof hash);
        log.append(&crc, sizeof crc);
        log.commit();
        ++report.events;
    }

    uint32_t crc = crc32c(kCrcStart, &kEndMarker, sizeof kEndMarker);
    crc = ~crc32c(crc, &report.events, sizeof report.events);
    log.append(&kEndMarker, sizeof kEndMarker);
    log.append(&report.events, sizeof report.events);
    log.append(&crc, sizeof crc);
    log.commit();
    log.finish();
    report.ring_full_waits = log.full_waits();
    report.disk_syncs = log.disk_syncs();
    report.max_sync_ns = log.max_sync_ns();
    return report;
}

namespace {

// Checks and hands over each record in order: (tick, book or nullptr, inputs, input count, recorded hash).
// Stops with Diverged as soon as visit returns false. events counts the records
// visit accepted, and the footer must agree with it.
template <typename Visit>
ReplayStatus walk_log(const std::string& log_path, uint64_t& events, Visit&& visit) {
    const MappedLog log(log_path);
    const char* pos = log.begin();
    const char* const end = log.end();

    if (static_cast<size_t>(end - pos) < sizeof kMagic) return ReplayStatus::TornTail;
    size_t tick_bytes = sizeof(Tick);
    bool has_book = true;
    if (std::memcmp(pos, kMagicV1, sizeof kMagicV1) == 0) {
        tick_bytes = kTickBytesV1;
        has_book = false;
    } else if (std::memcmp(pos, kMagicV2, sizeof kMagicV2) == 0) {
        tick_bytes = kTickBytesV2;
        has_book = false;
    } else if (std::memcmp(pos, kMagic, sizeof kMagic) != 0) {
        return ReplayStatus::Corrupt;
    }
    const size_t fixed_bytes = tick_bytes + kRecordBytesBesideTick + (has_book ? kBookCountBytes : 0);
    pos += sizeof kMagic;
    Book book;  // reused, so replay doesn't allocate on every event

    while (static_cast<size_t>(end - pos) >= sizeof(uint32_t)) {
        const uint32_t length = read_at<uint32_t>(pos);
        pos += sizeof length;

        if (length == kEndMarker) {
            if (static_cast<size_t>(end - pos) < kFooterBytes) return ReplayStatus::TornTail;
            const uint64_t recorded_events = read_at<uint64_t>(pos);
            const uint32_t crc = ~crc32c(crc32c(kCrcStart, &length, sizeof length), pos, sizeof recorded_events);
            const bool sound = crc == read_at<uint32_t>(pos + sizeof recorded_events) &&
                               recorded_events == events && pos + kFooterBytes == end;
            return sound ? ReplayStatus::Match : ReplayStatus::Corrupt;
        }
        if (length < fixed_bytes || length > kRingBytes || (length - fixed_bytes) % sizeof(uint64_t) != 0) {
            return ReplayStatus::Corrupt;  // no real record has this length
        }
        if (length > static_cast<size_t>(end - pos)) return ReplayStatus::TornTail;

        const size_t checked_bytes = length - sizeof(uint32_t);
        const uint32_t crc = ~crc32c(crc32c(kCrcStart, &length, sizeof length), pos, checked_bytes);
        if (crc != read_at<uint32_t>(pos + checked_bytes)) return ReplayStatus::Corrupt;
        size_t book_bytes = 0;
        uint32_t bid_levels = 0, ask_levels = 0;
        if (has_book) {
            bid_levels = read_at<uint32_t>(pos + tick_bytes);
            ask_levels = read_at<uint32_t>(pos + tick_bytes + sizeof bid_levels);
            if (bid_levels > kMaxBookLevels || ask_levels > kMaxBookLevels) return ReplayStatus::Corrupt;
            book_bytes = (size_t{bid_levels} + ask_levels) * sizeof(BookLevel);
            if (fixed_bytes + book_bytes > length) return ReplayStatus::Corrupt;
        }
        const char* const levels = pos + tick_bytes + (has_book ? kBookCountBytes : 0);
        const uint32_t count = read_at<uint32_t>(levels + book_bytes);
        if (fixed_bytes + book_bytes + size_t{count} * sizeof(uint64_t) != length) return ReplayStatus::Corrupt;

        Tick tick{};  // an older tick leaves the fields it didn't have at 0
        std::memcpy(&tick, pos, tick_bytes);
        if (tick.seq != events) return ReplayStatus::Corrupt;  // a record is missing or out of place
        book.bids.resize(bid_levels);
        book.asks.resize(ask_levels);
        std::memcpy(book.bids.data(), levels, bid_levels * sizeof(BookLevel));
        std::memcpy(book.asks.data(), levels + bid_levels * sizeof(BookLevel), ask_levels * sizeof(BookLevel));
        const char* const inputs = levels + book_bytes + sizeof count;
        const uint64_t recorded_hash = read_at<uint64_t>(pos + checked_bytes - sizeof(uint64_t));
        pos += length;

        if (!visit(tick, bid_levels + ask_levels ? &book : nullptr, inputs, count, recorded_hash)) {
            return ReplayStatus::Diverged;
        }
        ++events;
    }
    return ReplayStatus::TornTail;  // the end marker never came
}

// One strategy's turn at one recorded event. True if it matched the recording.
bool replays_exactly(Strategy& strategy, ReplayGate& gate, const Tick& tick, const Book* book, const char* inputs,
                     uint32_t count, uint64_t recorded_hash, int64_t& output) {
    gate.next_event(inputs, count);
    if (book) strategy.on_book(*book);
    output = strategy.on_tick(tick, gate);
    return event_hash(output, strategy.state_hash()) == recorded_hash && gate.read_exactly_all();
}

}  // namespace

ReplayReport replay_run(Strategy& strategy, const std::string& log_path) {
    ReplayGate gate;
    ReplayReport report;
    report.status = walk_log(log_path, report.events_replayed,
                             [&](const Tick& tick, const Book* book, const char* inputs, uint32_t count,
                                 uint64_t recorded_hash) {
                                 int64_t output = 0;
                                 if (replays_exactly(strategy, gate, tick, book, inputs, count, recorded_hash, output)) {
                                     return true;
                                 }
                                 report.first_divergence_seq = tick.seq;
                                 return false;
                             });
    return report;
}

ForkReport fork_run(Strategy& original, Strategy& fork, uint64_t fork_at, const std::function<void(Strategy&)>& change,
                    const std::string& log_path) {
    ReplayGate original_gate;
    ReplayGate fork_gate;
    ForkReport report;
    bool forked = false;
    const ReplayStatus log_status = walk_log(
        log_path, report.events,
        [&](const Tick& tick, const Book* book, const char* inputs, uint32_t count, uint64_t recorded_hash) {
            if (tick.seq == fork_at) {
                change(fork);
                forked = true;
            }
            // The original must match the recording the whole way, or nothing after it can be trusted.
            int64_t original_output = 0;
            if (!replays_exactly(original, original_gate, tick, book, inputs, count, recorded_hash, original_output)) {
                report.first_divergence_seq = tick.seq;
                return false;
            }
            int64_t fork_output = 0;
            if (!forked) {  // until the fork point, the fork must be the very same strategy in the same state
                if (replays_exactly(fork, fork_gate, tick, book, inputs, count, recorded_hash, fork_output)) return true;
                report.first_divergence_seq = tick.seq;
                return false;
            }
            fork_gate.next_event(inputs, count);
            if (book) fork.on_book(*book);
            fork_output = fork.on_tick(tick, fork_gate);
            if (fork_gate.overran()) {  // the recording has no value to give it
                report.fork_needed_more_inputs_seq = tick.seq;
                return false;
            }
            if (fork_output != original_output) {
                if (!report.first_difference_seq) report.first_difference_seq = tick.seq;
                ++report.different_events;
            }
            return true;
        });

    if (report.fork_needed_more_inputs_seq) {
        report.status = ForkStatus::NeedsMoreInputs;
    } else if (log_status == ReplayStatus::Diverged) {
        report.status = ForkStatus::Diverged;
    } else if (log_status == ReplayStatus::Corrupt) {
        report.status = ForkStatus::Corrupt;
    } else if (!forked) {
        report.status = ForkStatus::NeverForked;
    } else {
        report.status = log_status == ReplayStatus::Match ? ForkStatus::Done : ForkStatus::TornTail;
    }
    return report;
}

}  // namespace fr
