// Unit tests for Real Ravi. Each one drives the strategy with hand-made ticks.
#include <cstdio>
#include <string>

#include "real_ravi.h"

using fr_ravi::Quotes;
using fr_ravi::RealRavi;
using fr_ravi::Settings;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}

class TestGate final : public fr::Gate {
public:
    uint64_t now_ns() override { return now; }
    uint64_t random_u64() override { return 0; }
    uint64_t now = 1'000 * fr_ravi::kNanosPerSecond;
};

constexpr int64_t cents(int64_t c) { return c * fr_ravi::kUnitsPerCent; }

fr::Tick book(int64_t bid, int64_t ask) { return {0, 0, 0, 0, cents(bid), cents(ask)}; }
fr::Tick trade(int64_t side, int64_t price, int64_t bid, int64_t ask) {
    return {0, cents(price), 100'000'000, side, cents(bid), cents(ask)};
}

// The numbers from the strategy doc: mid $78,464.00, bid $78,460.13, ask $78,463.87.
void worked_example_matches_the_doc() {
    const Quotes q = fr_ravi::make_quotes(7'846'300, 7'846'500, 1, 40'000, 4'000, Settings{});
    check(q.bid == 7'846'013, "worked example: bid is $78,460.13");
    check(q.ask == 7'846'387, "worked example: ask is $78,463.87");
}

void quotes_never_cross() {
    const Quotes heavy = fr_ravi::make_quotes(1'000'000, 1'000'002, 4, 400'000, 0, Settings{});
    check(heavy.ask == 1'000'001, "a long position can't sell into the best bid");
    const Quotes short_ = fr_ravi::make_quotes(1'000'000, 1'000'002, -4, 400'000, 0, Settings{});
    check(short_.bid == 1'000'001, "a short position can't buy into the best ask");
}

void inventory_pushes_prices_down() {
    const Quotes flat = fr_ravi::make_quotes(1'000'000, 1'002'000, 0, 40'000, 0, Settings{});
    const Quotes long_ = fr_ravi::make_quotes(1'000'000, 1'002'000, 3, 40'000, 0, Settings{});
    const Quotes short_ = fr_ravi::make_quotes(1'000'000, 1'002'000, -3, 40'000, 0, Settings{});
    check(long_.bid < flat.bid, "holding stock lowers the bid");
    check(short_.ask >= flat.ask, "being short doesn't lower the ask");
    check(short_.bid >= flat.bid, "being short raises the bid");
}

void inventory_limit_stops_one_side() {
    const Settings s;
    const Quotes full = fr_ravi::make_quotes(1'000'000, 1'000'001, s.max_inventory_lots, 0, 0, s);
    check(!full.bid_on && full.ask_on, "full long: stops buying, keeps selling");
    const Quotes empty = fr_ravi::make_quotes(1'000'000, 1'000'001, -s.max_inventory_lots, 0, 0, s);
    check(empty.bid_on && !empty.ask_on, "full short: stops selling, keeps buying");
}

void buying_pressure_leans_up() {
    const Quotes calm = fr_ravi::make_quotes(1'000'000, 1'002'000, 0, 0, 0, Settings{});
    const Quotes buying = fr_ravi::make_quotes(1'000'000, 1'002'000, 0, 0, 10'000, Settings{});
    check(buying.bid > calm.bid, "buying pressure raises the bid");
}

void fills_are_strict() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    const int64_t ask = ravi.quotes().ask;
    ravi.on_tick(trade(+1, ask, 1'000'000, 1'000'002), gate);
    check(ravi.fills() == 0, "a trade at our price doesn't fill: others queued first");
    const int64_t ask2 = ravi.quotes().ask;
    ravi.on_tick(trade(+1, ask2 + 1, 1'000'000, 1'000'002), gate);
    check(ravi.fills() == 1 && ravi.lots() == -1, "a trade through our ask sells one lot");
}

void buyers_never_fill_our_bid() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    ravi.on_tick(trade(+1, 1, 1'000'000, 1'000'002), gate);
    check(ravi.fills() == 0, "a buyer can't fill our bid");
}

void round_trip_profit_and_fees() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'000), gate);
    const int64_t bid = ravi.quotes().bid;
    ravi.on_tick(trade(-1, bid - 1, 1'000'000, 1'000'000), gate);
    const int64_t ask = ravi.quotes().ask;
    ravi.on_tick(trade(+1, ask + 1, 1'000'000, 1'000'000), gate);
    check(ravi.lots() == 0 && ravi.fills() == 2, "round trip ends flat");
    check(ravi.gross_pnl() == ask - bid, "gross profit is the spread earned");
    check(ravi.fees() == bid / 1'000 + ask / 1'000, "fees are 0.1% each way");
    check(ravi.net_pnl() == ravi.gross_pnl() - ravi.fees(), "net is gross minus fees");
}

void loss_limit_stops_for_the_day() {
    Settings s;
    s.max_loss_millicents = 100;
    RealRavi ravi(s);
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'000), gate);
    const int64_t bid = ravi.quotes().bid;
    ravi.on_tick(trade(-1, bid - 1, 1'000'000, 1'000'000), gate);
    check(ravi.stopped() && !ravi.quotes().bid_on && !ravi.quotes().ask_on, "a loss past the limit pulls all quotes");
    ravi.on_tick(book(2'000'000, 2'000'000), gate);
    check(ravi.stopped() && !ravi.quotes().ask_on, "it stays stopped even if the price recovers");
}

void stale_book_pulls_quotes() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    check(ravi.quotes().bid_on, "fresh book: quoting");
    gate.now += 6 * fr_ravi::kNanosPerSecond;
    ravi.on_tick(trade(+1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(!ravi.quotes().bid_on && !ravi.quotes().ask_on, "6 seconds without a book update pulls quotes");
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    check(ravi.quotes().bid_on, "a fresh book resumes quoting");
}

void wild_market_pulls_quotes() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    for (int i = 0; i < 40; ++i) {
        gate.now += fr_ravi::kNanosPerSecond;
        const int64_t mid = i % 2 ? 1'000'000 : 1'020'000;
        ravi.on_tick(book(mid, mid + 2), gate);
    }
    check(ravi.variance() > 1'000'000 && !ravi.quotes().bid_on, "the volatility brake pulls quotes");
}

void imbalance_stays_in_range() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    for (int i = 0; i < 500; ++i) ravi.on_tick(trade(+1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(ravi.imbalance_bp() > 9'000 && ravi.imbalance_bp() <= 10'000, "all buying gives an imbalance near +1");
    for (int i = 0; i < 500; ++i) ravi.on_tick(trade(-1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(ravi.imbalance_bp() < -9'000 && ravi.imbalance_bp() >= -10'000, "all selling gives an imbalance near -1");
}

void step_aside_pulls_the_pushed_side() {
    Settings s;
    s.step_aside_bp = 3'000;
    RealRavi ravi(s);
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    for (int i = 0; i < 100; ++i) ravi.on_tick(trade(+1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(!ravi.quotes().ask_on && ravi.quotes().bid_on, "buyers pushing: Ravi stops selling, keeps buying");
    for (int i = 0; i < 200; ++i) ravi.on_tick(trade(-1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(ravi.quotes().ask_on && !ravi.quotes().bid_on, "sellers pushing: Ravi stops buying, keeps selling");
}

void rush_pulls_both_sides_until_it_passes() {
    Settings s;
    s.rush_trades = 50;
    RealRavi ravi(s);
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    for (int i = 0; i < 49; ++i) ravi.on_tick(trade(i % 2 ? 1 : -1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(ravi.quotes().bid_on && ravi.quotes().ask_on, "49 trades in a second: still quoting");
    ravi.on_tick(trade(1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(!ravi.quotes().bid_on && !ravi.quotes().ask_on, "50 trades in a second: both sides pulled");
    gate.now += 2 * fr_ravi::kNanosPerSecond;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    check(ravi.quotes().bid_on && ravi.quotes().ask_on, "two quiet seconds later: quoting again");
}

void new_rules_are_off_by_default() {
    RealRavi ravi;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'002), gate);
    for (int i = 0; i < 500; ++i) ravi.on_tick(trade(+1, 1'000'001, 1'000'000, 1'000'002), gate);
    check(ravi.quotes().ask_on && ravi.quotes().bid_on, "default Ravi keeps both sides in a push and a rush");
}

void chase_shifts_quotes_after_a_fill() {
    Settings s;
    s.chase_cents = 300;
    RealRavi ravi(s);
    RealRavi plain;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'800), gate);
    plain.on_tick(book(1'000'000, 1'000'800), gate);
    const int64_t ask = ravi.quotes().ask;
    ravi.on_tick(trade(+1, ask + 1, 1'000'000, 1'000'800), gate);  // a buyer runs through our ask
    plain.on_tick(trade(+1, ask + 1, 1'000'000, 1'000'800), gate);
    check(ravi.lots() == -1, "chase: the buyer filled our ask");
    check(ravi.quotes().ask == plain.quotes().ask + 300 && ravi.quotes().bid == plain.quotes().bid + 300,
          "chase: both quotes move up $3 after selling");
    gate.now += 4 * fr_ravi::kNanosPerSecond;
    ravi.on_tick(book(1'000'000, 1'000'800), gate);
    plain.on_tick(book(1'000'000, 1'000'800), gate);
    check(ravi.quotes().ask == plain.quotes().ask, "chase: back to normal after 3 seconds");

    RealRavi tight(s);  // in a 2-cent book the $3 shift would cross, so it stops at the touch
    tight.on_tick(book(1'000'000, 1'000'002), gate);
    tight.on_tick(trade(+1, tight.quotes().ask + 1, 1'000'000, 1'000'002), gate);
    check(tight.quotes().bid == 1'000'001, "chase: a shifted bid never crosses the best ask");
}

void ladder_steps_the_hit_side_only() {
    Settings s;
    s.ladder_cents = 300;
    RealRavi ravi(s);
    RealRavi plain;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'800), gate);
    plain.on_tick(book(1'000'000, 1'000'800), gate);
    const int64_t first = ravi.quotes().ask;
    ravi.on_tick(trade(+1, first + 1, 1'000'000, 1'000'800), gate);  // sold at `first`
    plain.on_tick(trade(+1, first + 1, 1'000'000, 1'000'800), gate);
    check(ravi.quotes().ask == first + 300, "ladder: after selling, the ask sits $3 above the fill");
    check(ravi.quotes().bid == plain.quotes().bid, "ladder: the bid side is left alone");
    ravi.on_tick(trade(+1, first + 301, 1'000'000, 1'000'800), gate);  // sold again one step up
    check(ravi.lots() == -2 && ravi.quotes().ask == first + 600, "ladder: a second sale steps $3 higher again");
    gate.now += 4 * fr_ravi::kNanosPerSecond;
    ravi.on_tick(book(1'000'000, 1'000'800), gate);
    check(ravi.quotes().ask < first + 300, "ladder: the step ends after its set time");
}

void ladder_mirrors_for_buying() {
    Settings s;
    s.ladder_cents = 300;
    RealRavi ravi(s);
    RealRavi plain;
    TestGate gate;
    ravi.on_tick(book(1'000'000, 1'000'800), gate);
    plain.on_tick(book(1'000'000, 1'000'800), gate);
    const int64_t bought = ravi.quotes().bid;
    ravi.on_tick(trade(-1, bought - 1, 1'000'000, 1'000'800), gate);  // a seller runs through our bid
    plain.on_tick(trade(-1, bought - 1, 1'000'000, 1'000'800), gate);
    check(ravi.lots() == 1 && ravi.quotes().bid == bought - 300, "ladder: after buying, the bid sits $3 below the fill");
    check(ravi.quotes().ask == plain.quotes().ask, "ladder: after buying, the ask side is left alone");
}

}  // namespace

int main() {
    worked_example_matches_the_doc();
    quotes_never_cross();
    inventory_pushes_prices_down();
    inventory_limit_stops_one_side();
    buying_pressure_leans_up();
    fills_are_strict();
    buyers_never_fill_our_bid();
    round_trip_profit_and_fees();
    loss_limit_stops_for_the_day();
    stale_book_pulls_quotes();
    wild_market_pulls_quotes();
    imbalance_stays_in_range();
    step_aside_pulls_the_pushed_side();
    rush_pulls_both_sides_until_it_passes();
    new_rules_are_off_by_default();
    chase_shifts_quotes_after_a_fill();
    ladder_steps_the_hit_side_only();
    ladder_mirrors_for_buying();
    std::printf("%d failed\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
