// Real Ravi: a market maker with inventory control and an order-flow lean.
// Quotes follow Avellaneda-Stoikov, and the lean is our own addition.
// Paper trades only: fills are simulated, and no order ever leaves this file.
//
// Everything is a whole number, so replay matches bit for bit:
//   prices in cents, variance in cents squared per second, inventory in lots
//   (1 lot = 0.001 BTC), and money in millicents (1/1000 of a cent).
// One lot at a price of P cents is therefore worth exactly P millicents.
#pragma once

#include <cstdint>
#include <deque>

#include "fr/flight_recorder.h"

namespace fr_ravi {

constexpr int64_t kUnitsPerCent = 1'000'000;  // feed prices are scaled by 100,000,000
constexpr uint64_t kNanosPerSecond = 1'000'000'000;

struct Settings {
    int64_t window_seconds = 60;           // T - t, the risk window
    int64_t risk_divisor = 10'000;         // gamma = 0.01 per dollar, expressed for cents
    int64_t base_spread_cents = 133;       // (2 / gamma) * ln(1 + gamma / k), with k = 1.5
    int64_t lean_cents = 100;              // beta = $1
    int64_t max_inventory_lots = 5;        // inventory limit
    int64_t max_loss_millicents = 1'000'000;  // loss limit: $10, counted after fees
    int64_t max_variance = 1'000'000;      // volatility brake: sigma of $10 per root second
    uint64_t stale_ns = 5 * kNanosPerSecond;  // stale data stop
    int64_t fee_divisor = 1'000;           // 0.1% of notional per fill
    int64_t step_aside_bp = 0;             // pull the side a strong push would run over; 0 is off
    int64_t rush_trades = 0;               // pull both sides after this many trades in a second; 0 is off
    int64_t chase_cents = 0;               // after a fill, shift both quotes this far the way the price went; 0 is off
    uint64_t chase_ns = 3 * kNanosPerSecond;  // and for this long
    int64_t ladder_cents = 0;              // after a fill, the side hit stays at least this far past the fill price; 0 is off
    uint64_t ladder_ns = 3 * kNanosPerSecond;  // and for this long after each fill
};

struct Quotes {
    bool bid_on = false;
    bool ask_on = false;
    int64_t bid = 0;
    int64_t ask = 0;
};

// imbalance_bp runs from -10,000 (all selling) to +10,000 (all buying).
inline Quotes make_quotes(int64_t best_bid, int64_t best_ask, int64_t lots, int64_t variance, int64_t imbalance_bp,
                          const Settings& s) {
    const int64_t risk = variance * s.window_seconds / s.risk_divisor;  // gamma * sigma^2 * (T - t)
    const int64_t spread = risk + s.base_spread_cents;                  // delta
    const int64_t lean = s.lean_cents * imbalance_bp / 10'000;          // beta * imbalance

    // Doubled so that halves of a cent stay exact: this is 2 * r'.
    const int64_t centre = best_bid + best_ask - 2 * lots * risk + 2 * lean;

    Quotes q;
    q.bid = (centre - spread) / 2;      // rounded down, away from the middle
    q.ask = (centre + spread + 1) / 2;  // rounded up, away from the middle
    if (q.bid >= best_ask) q.bid = best_ask - 1;  // never cross: a quote must wait, not trade at once
    if (q.ask <= best_bid) q.ask = best_bid + 1;
    q.bid_on = lots < s.max_inventory_lots;   // a full position stops adding to itself
    q.ask_on = lots > -s.max_inventory_lots;
    return q;
}

class RealRavi final : public fr::Strategy {
public:
    explicit RealRavi(const Settings& settings = Settings{}) : s_(settings) {}

    int64_t on_tick(const fr::Tick& tick, fr::Gate& gate) override {
        const uint64_t now = gate.now_ns();
        now_ = now;
        const int64_t best_bid = tick.best_bid / kUnitsPerCent;
        const int64_t best_ask = tick.best_ask / kUnitsPerCent;
        const int64_t mid2 = best_bid + best_ask;  // twice the mid price

        if (tick.side == 0) {
            last_book_ns_ = now;
        } else {
            fill_against(tick);
            update_flow(tick);
            if (s_.rush_trades > 0) recent_trades_.push_back(now);
        }
        while (!recent_trades_.empty() && recent_trades_.front() + kNanosPerSecond < now) recent_trades_.pop_front();
        update_variance(mid2, now);
        mark_ = mid2 / 2;

        if (net_pnl() < -s_.max_loss_millicents) stopped_ = true;  // stays stopped for the day

        quotes_ = make_quotes(best_bid, best_ask, lots_, variance_, imbalance_bp(), s_);
        if (s_.step_aside_bp > 0) {  // buyers pushing: don't sell to them, and the same for sellers
            if (imbalance_bp() > s_.step_aside_bp) quotes_.ask_on = false;
            if (imbalance_bp() < -s_.step_aside_bp) quotes_.bid_on = false;
        }
        if (s_.chase_cents > 0 && now < chase_until_) {  // bet that the move which filled us carries on
            quotes_.bid += chase_dir_ * s_.chase_cents;
            quotes_.ask += chase_dir_ * s_.chase_cents;
            if (quotes_.bid >= best_ask) quotes_.bid = best_ask - 1;  // still never cross
            if (quotes_.ask <= best_bid) quotes_.ask = best_bid + 1;
        }
        if (s_.ladder_cents > 0) {  // after selling at P, sell no lower than P + step for a while; buying mirrors it
            if (now < ask_floor_until_ && quotes_.ask < ask_floor_) quotes_.ask = ask_floor_;
            if (now < bid_cap_until_ && quotes_.bid > bid_cap_) quotes_.bid = bid_cap_;
        }
        if (s_.rush_trades > 0 && static_cast<int64_t>(recent_trades_.size()) >= s_.rush_trades) {
            quotes_.bid_on = quotes_.ask_on = false;  // hide until the rush passes
        }
        if (stopped_ || variance_ > s_.max_variance || now - last_book_ns_ > s_.stale_ns) {
            quotes_.bid_on = quotes_.ask_on = false;
        }
        return quotes_.bid * 3 + quotes_.ask * 5 + (quotes_.bid_on ? 1 : 0) + (quotes_.ask_on ? 2 : 0);
    }

    uint64_t state_hash() const override {
        uint64_t h = 0xCBF29CE484222325ull;
        for (int64_t v : {lots_, cash_, fees_, variance_, bucket_, flow_, volume_, fills_, mark_, last_mid2_}) {
            h = (h ^ static_cast<uint64_t>(v)) * 0x100000001B3ull;
        }
        return (h ^ last_second_) * 0x100000001B3ull;
    }

    // For forks: the same Ravi, in the same state, from here on with new settings.
    void change_settings(const Settings& settings) { s_ = settings; }
    const Settings& settings() const { return s_; }

    int64_t imbalance_bp() const { return volume_ == 0 ? 0 : flow_ * 10'000 / volume_; }
    int64_t gross_pnl() const { return cash_ + lots_ * mark_; }  // mark to market
    int64_t net_pnl() const { return gross_pnl() - fees_; }
    int64_t lots() const { return lots_; }
    int64_t fills() const { return fills_; }
    int64_t fees() const { return fees_; }
    int64_t variance() const { return variance_; }
    int64_t max_lots() const { return max_lots_; }
    bool stopped() const { return stopped_; }
    const Quotes& quotes() const { return quotes_; }

private:
    // Strict: a trade must go through our price, so anyone queued there went first.
    void fill_against(const fr::Tick& tick) {
        const int64_t price = tick.price / kUnitsPerCent;
        if (tick.side > 0 && quotes_.ask_on && price > quotes_.ask) trade(-1, quotes_.ask);
        if (tick.side < 0 && quotes_.bid_on && price < quotes_.bid) trade(+1, quotes_.bid);
    }

    void trade(int64_t bought_lots, int64_t price) {
        lots_ += bought_lots;
        cash_ -= bought_lots * price;
        fees_ += price / s_.fee_divisor;
        ++fills_;
        chase_dir_ = -bought_lots;  // we sold, so buyers are pushing the price up
        chase_until_ = now_ + s_.chase_ns;
        if (bought_lots < 0) {  // each fill on a side moves that side's step from its own price
            ask_floor_ = price + s_.ladder_cents;
            ask_floor_until_ = now_ + s_.ladder_ns;
        } else {
            bid_cap_ = price - s_.ladder_cents;
            bid_cap_until_ = now_ + s_.ladder_ns;
        }
        const int64_t held = lots_ < 0 ? -lots_ : lots_;
        if (held > max_lots_) max_lots_ = held;
    }

    // Exponentially weighted, so recent trades count most.
    void update_flow(const fr::Tick& tick) {
        const int64_t qty = tick.qty / kUnitsPerCent + 1;  // whole units, and never zero
        flow_ += tick.side * qty - flow_ / 32;
        volume_ += qty - volume_ / 32;
    }

    // Adds up squared mid moves for each second, then folds each finished
    // second into a running average. mid2 is doubled, so moves are divided by 2.
    void update_variance(int64_t mid2, uint64_t now) {
        if (last_mid2_ != 0) {
            const int64_t move = (mid2 - last_mid2_) / 2;
            bucket_ += move * move;
        }
        last_mid2_ = mid2;

        const uint64_t second = now / kNanosPerSecond;
        if (last_second_ == 0) last_second_ = second;
        uint64_t passed = second - last_second_;
        if (passed > 60) passed = 60;  // after a long gap, older seconds no longer matter
        for (uint64_t i = 0; i < passed; ++i) {
            variance_ += (bucket_ - variance_) / 16;
            bucket_ = 0;
        }
        last_second_ = second;
    }

    Settings s_;
    Quotes quotes_;
    int64_t lots_ = 0;
    int64_t max_lots_ = 0;
    int64_t cash_ = 0;
    int64_t fees_ = 0;
    int64_t fills_ = 0;
    int64_t mark_ = 0;
    int64_t variance_ = 0;
    int64_t bucket_ = 0;
    int64_t flow_ = 0;
    int64_t volume_ = 0;
    int64_t last_mid2_ = 0;
    uint64_t last_second_ = 0;
    uint64_t last_book_ns_ = 0;
    uint64_t now_ = 0;
    uint64_t chase_until_ = 0;
    int64_t chase_dir_ = 0;
    int64_t ask_floor_ = 0;
    uint64_t ask_floor_until_ = 0;
    int64_t bid_cap_ = 0;
    uint64_t bid_cap_until_ = 0;
    std::deque<uint64_t> recent_trades_;  // trade times in the last second, only when rush_trades is on
    bool stopped_ = false;
};

}  // namespace fr_ravi
