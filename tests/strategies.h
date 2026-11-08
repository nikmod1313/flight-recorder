// Toy strategies. Clean uses the gate properly. Each leak variant behaves like
// Clean until seq == leak_at, so a test knows exactly where divergence must be.
#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "fr/flight_recorder.h"
#include "real_ravi.h"

namespace fr_test {

class Clean : public fr::Strategy {
public:
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        fold(gate.now_ns());
        fold(gate.random_u64());
        fold(static_cast<uint64_t>(tick.price));
        fold(static_cast<uint64_t>(tick.qty));
        fold(static_cast<uint64_t>(tick.side));
        fold(static_cast<uint64_t>(tick.best_bid));
        fold(static_cast<uint64_t>(tick.best_ask));
        return static_cast<int64_t>(state_ >> 1);
    }
    uint64_t state_hash() const override { return state_; }

protected:
    void fold(uint64_t v) {
        state_ ^= v;
        state_ *= 0x100000001B3ull;
    }
    uint64_t state_ = 0xCBF29CE484222325ull;
};

class WallClockLeak final : public Clean {
public:
    explicit WallClockLeak(uint64_t leak_at) : leak_at_(leak_at) {}
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        int64_t out = Clean::on_tick(tick, gate);
        if (tick.seq < leak_at_) return out;
        auto now = std::chrono::system_clock::now().time_since_epoch();
        fold(static_cast<uint64_t>(now.count()));
        return static_cast<int64_t>(state_ >> 1);
    }

private:
    uint64_t leak_at_;
};

class RandomLeak final : public Clean {
public:
    explicit RandomLeak(uint64_t leak_at) : leak_at_(leak_at) {}
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        int64_t out = Clean::on_tick(tick, gate);
        if (tick.seq < leak_at_) return out;
        fold((static_cast<uint64_t>(device_()) << 32) | device_());
        return static_cast<int64_t>(state_ >> 1);
    }

private:
    uint64_t leak_at_;
    std::random_device device_;
};

// Pointer keys hash by heap address, so iteration order changes between
// processes. Integer keys would not leak: their order repeats on every run.
// The padding varies addresses even where the allocator is otherwise repeatable.
class IterationLeak final : public Clean {
public:
    explicit IterationLeak(uint64_t leak_at) : leak_at_(leak_at) {
        padding_.resize(std::random_device{}() % 4096 + 1);
        for (auto& block : padding_) block = std::make_unique<uint64_t>(0);
        for (uint64_t i = 0; i < 64; ++i) {
            nodes_.push_back(std::make_unique<uint64_t>(i));
            set_.insert(nodes_.back().get());
        }
    }
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        int64_t out = Clean::on_tick(tick, gate);
        if (tick.seq < leak_at_) return out;
        for (const uint64_t* node : set_) fold(*node);
        return static_cast<int64_t>(state_ >> 1);
    }

private:
    uint64_t leak_at_;
    std::vector<std::unique_ptr<uint64_t>> padding_;
    std::vector<std::unique_ptr<uint64_t>> nodes_;
    std::unordered_set<const uint64_t*> set_;
};

// Reads one gate input and throws it away, on one side of the run only. The
// output and state stay identical, so only the gate can notice.
class ExtraRead final : public Clean {
public:
    ExtraRead(uint64_t at, bool on_this_side) : at_(at), on_this_side_(on_this_side) {}
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        int64_t out = Clean::on_tick(tick, gate);
        if (on_this_side_ && tick.seq == at_) (void)gate.random_u64();  // last, so no other read shifts
        return out;
    }

private:
    uint64_t at_;
    bool on_this_side_;
};

// Dies like a killed process: no destructors, no flushes.
class CrashAt final : public Clean {
public:
    explicit CrashAt(uint64_t at) : at_(at) {}
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        if (tick.seq == at_) _exit(42);
        return Clean::on_tick(tick, gate);
    }

private:
    uint64_t at_;
};

// Real Ravi timed by his own stopwatch instead of the gate. The first time
// still comes from the gate, so only a run where seconds pass can show the
// leak: replay runs far faster than the live feed did.
class RaviStopwatchLeak final : public fr::Strategy {
public:
    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        Stopwatch leaky(gate, *this);
        return ravi_.on_tick(tick, leaky);
    }
    uint64_t state_hash() const override { return ravi_.state_hash(); }

private:
    struct Stopwatch final : fr::Gate {
        Stopwatch(fr::Gate& gate, RaviStopwatchLeak& owner) : gate_(gate), owner_(owner) {}
        uint64_t now_ns() override {
            const uint64_t recorded = gate_.now_ns();  // still read, so the input count matches
            const auto now = std::chrono::steady_clock::now();
            if (owner_.first_ns_ == 0) {
                owner_.first_ns_ = recorded;
                owner_.started_ = now;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - owner_.started_);
            return owner_.first_ns_ + static_cast<uint64_t>(elapsed.count());
        }
        uint64_t random_u64() override { return gate_.random_u64(); }
        fr::Gate& gate_;
        RaviStopwatchLeak& owner_;
    };

    fr_ravi::RealRavi ravi_;
    uint64_t first_ns_ = 0;
    std::chrono::steady_clock::time_point started_;
};

inline std::unique_ptr<fr::Strategy> make_strategy(const std::string& name, uint64_t at, bool replaying) {
    if (name == "clean") return std::make_unique<Clean>();
    if (name == "ravi") return std::make_unique<fr_ravi::RealRavi>();
    if (name == "ravi_stopwatch") return std::make_unique<RaviStopwatchLeak>();
    if (name == "wall_clock") return std::make_unique<WallClockLeak>(at);
    if (name == "random") return std::make_unique<RandomLeak>(at);
    if (name == "iteration") return std::make_unique<IterationLeak>(at);
    if (name == "extra_read") return std::make_unique<ExtraRead>(at, replaying);
    if (name == "skipped_read") return std::make_unique<ExtraRead>(at, !replaying);
    if (name == "crash") return std::make_unique<CrashAt>(at);
    return nullptr;
}

}  // namespace fr_test
