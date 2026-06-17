/*
 * test_risk.cpp - standalone unit tests for RiskEngine (src/native/risk.cc).
 *
 * Links risk.cc DIRECTLY - no N-API, no addon, no test-only hooks in the shipped
 * library. Exercises the safety-critical pre-trade controls (the same logic the
 * addon enforces on every order) plus the DBL_MAX/sanePositive guards, in C++
 * isolation. Build + run via `npm run test:cpp`.
 */
#include "risk.h"

#include <limits>
#include <string>
#include <vector>

#include "check.h"

using namespace ctp;
static const double DMAX = std::numeric_limits<double>::max(); // CTP "unset" sentinel band

// Mock reference-price source (stands in for the MarketData snapshot cache, so
// the deviation-reference tests need no CTP md struct).
struct MockSnap : RefPriceSource {
  std::unordered_map<std::string, double> px;
  double last(const std::string &id) const override {
    auto it = px.find(id);
    return it != px.end() ? it->second : 0.0;
  }
};

int main() {
  // --- check(): per-order limits ---
  {
    RiskEngine r;
    RiskConfig c;
    c.maxOrderVolume = 10;
    r.configure(c);
    CHECK(r.check("rb", 3500, 0, 10).ok, "maxOrderVolume: at the cap is allowed");
    CHECK(!r.check("rb", 3500, 0, 11).ok, "maxOrderVolume: over the cap is blocked");
    CHECK(r.lastBlockReason() != nullptr, "lastBlockReason is set after a block");
  }
  {
    RiskEngine r;
    RiskConfig c;
    c.maxNotional = 1000000;
    r.configure(c);
    r.setMultiplier("rb", 10);
    CHECK(!r.check("rb", 3500, 0, 30).ok, "maxNotional: 3500*30*10=1.05M over 1M is blocked (multiplier-aware)");
    CHECK(r.check("rb", 3500, 0, 28).ok, "maxNotional: 3500*28*10=0.98M under 1M is allowed");
  }
  {
    RiskEngine r;
    RiskConfig c;
    c.maxPriceDeviation = 0.02;
    r.configure(c);
    CHECK(!r.check("rb", 3600, 3500, 1).ok, "maxPriceDeviation: 2.86% over 2% is blocked");
    CHECK(r.check("rb", 3550, 3500, 1).ok, "maxPriceDeviation: 1.43% under 2% is allowed");
    CHECK(r.check("rb", 9999, 0, 1).ok, "maxPriceDeviation: refPrice 0 skips the check");
  }
  {
    RiskEngine r;
    RiskConfig c;
    c.maxOrderVolume = 10;
    r.configure(c);
    r.halt();
    CHECK(!r.check("rb", 3500, 0, 1).ok, "kill-switch: halt blocks any order");
    CHECK(r.isHalted(), "isHalted() is true after halt()");
    r.resume();
    CHECK(r.check("rb", 3500, 0, 1).ok, "kill-switch: resume allows orders again");
  }
  {
    RiskEngine r; // all-zero config = every control disabled
    CHECK(r.check("rb", 1e9, 0, 1000000).ok, "disabled config: nothing is blocked");
  }

  // --- rate limiter (token bucket) ---
  {
    RiskEngine r;
    RiskConfig c;
    c.maxOrdersPerSec = 1000; // high rate so ~no refill within the test instant
    c.orderBurst = 3;
    r.configure(c);
    int allowed = 0;
    for (int i = 0; i < 8; i++)
      if (r.allowRate())
        allowed++;
    CHECK(allowed == 3, "rate limiter: exactly the burst (3) is allowed, the rest throttled");
  }

  // --- reference price from the attached MD snapshot (deviation reference) ---
  {
    RiskEngine r;
    CHECK(r.refPrice("rb") == 0.0, "no snapshot attached -> refPrice 0 (deviation skipped)");
    auto snap = std::make_shared<MockSnap>();
    snap->px["rb"] = 3500;
    r.setMdSnapshot(snap);
    CHECK(r.refPrice("rb") == 3500, "refPrice reads the attached MD snapshot");
    CHECK(r.refPrice("zz") == 0.0, "refPrice of an instrument absent from the snapshot is 0");
    snap->px["bad"] = DMAX; // CTP DBL_MAX "unset" sentinel
    CHECK(r.refPrice("bad") == 0.0, "refPrice rejects a DBL_MAX 'unset' snapshot price");
    r.setMdSnapshot(nullptr);
    CHECK(r.refPrice("rb") == 0.0, "detached snapshot -> refPrice 0 again");
  }

  // --- held position cost (onTrade) + global maxPositionCost cap ---
  {
    RiskEngine r;
    RiskConfig c;
    c.maxPositionCost = 100000;
    r.configure(c);
    r.setMultiplier("rb", 10);
    r.onTrade("rb", true, true, 3500, 2); // open long 2 @3500 x10 = 70000 held
    CHECK(r.currentPositionCost() == 70000, "onTrade(open) accumulates held cost (70000)");
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 1) == OpenGate::CostLimit,
          "global cost cap: held 70000 + 35000 reserved > 100000 is blocked");
    r.onTrade("rb", false, false, 3600, 2); // close the long 2
    CHECK(r.currentPositionCost() < 1.0, "onTrade(close) releases the held cost (~0)");
  }

  // --- margin rate: tracked cost becomes real MARGIN = notional * marginRate ---
  {
    RiskEngine r;
    r.setMultiplier("rb", 10);
    r.setMarginRate("rb", 0.1);            // 10% margin
    r.onTrade("rb", true, true, 3500, 2);  // 3500*2*10*0.1 = 7000 margin (vs 70000 notional)
    CHECK(r.currentPositionCost() == 7000, "marginRate scales held cost to real margin (7000)");
  }
  {
    RiskEngine r; // unset margin rate -> default 1.0 -> cost == notional (back-compatible)
    r.setMultiplier("rb", 10);
    r.onTrade("rb", true, true, 3500, 2);
    CHECK(r.currentPositionCost() == 70000, "unset marginRate defaults to notional (1.0) - back-compatible");
  }
  {
    RiskEngine r; // global maxMargin cap (in margin units)
    RiskConfig c;
    c.maxMargin = 10000;
    r.configure(c);
    r.setMultiplier("rb", 10);
    r.setMarginRate("rb", 0.1);
    r.onTrade("rb", true, true, 3500, 2); // 7000 margin held
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 1) == OpenGate::CostLimit,
          "maxMargin: held 7000 + 3500 reserved > 10000 is blocked (margin-based)");
    CHECK(r.tryReserveOpen("o2", "rb", true, 2000, 1) == OpenGate::Ok,
          "maxMargin: held 7000 + 2000 reserved = 9000 < 10000 is allowed");
  }
  {
    RiskEngine r; // maxMargin takes precedence over the deprecated maxPositionCost alias
    RiskConfig c;
    c.maxMargin = 5000;
    c.maxPositionCost = 999999; // alias would allow; maxMargin must win
    r.configure(c);
    r.setMultiplier("rb", 10);
    r.setMarginRate("rb", 0.1);
    CHECK(r.tryReserveOpen("o", "rb", true, 6000, 1) == OpenGate::CostLimit,
          "maxMargin (5000) overrides maxPositionCost: 6000*1*10*0.1=6000 > 5000 blocked");
  }
  {
    RiskEngine r; // setMarginRate ignores invalid (0 / negative / sentinel) -> default stays
    r.setMultiplier("rb", 10);
    r.setMarginRate("rb", 0);
    r.setMarginRate("rb", -0.5);
    r.setMarginRate("rb", DMAX);
    r.onTrade("rb", true, true, 3500, 1); // default 1.0 -> 35000 notional, never zeroed
    CHECK(r.currentPositionCost() == 35000, "setMarginRate ignores 0/negative/sentinel (default kept, margin never voided)");
  }
  {
    RiskEngine r; // marginRate is static metadata: survives resetPositions (like multipliers)
    r.setMultiplier("rb", 10);
    r.setMarginRate("rb", 0.1);
    r.onTrade("rb", true, true, 3500, 1);
    r.resetPositions();
    r.onTrade("rb", true, true, 3500, 1); // 3500*1*10*0.1 = 3500
    CHECK(r.currentPositionCost() == 3500, "marginRate survives resetPositions (fill still scaled x0.1)");
  }

  // --- per-instrument cost cap ---
  {
    RiskEngine r;
    r.setMultiplier("ag", 15);
    r.setMaxInstrumentCost("ag", 50000);
    CHECK(r.tryReserveOpen("o1", "ag", true, 5000, 1) == OpenGate::CostLimit,
          "per-instrument cost cap: 5000*1*15=75000 > 50000 is blocked");
    CHECK(r.tryReserveOpen("o2", "ag", true, 3000, 1) == OpenGate::Ok,
          "per-instrument cost cap: 3000*1*15=45000 < 50000 is allowed");
  }

  // --- per-side lot cap + in-flight reservation ---
  {
    RiskEngine r;
    r.setMaxPositionVolume("rb", true, 2); // long capped at 2 lots
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 2) == OpenGate::Ok, "lot cap: reserve 2 long = at cap, ok");
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "lot cap: 2 reserved + 1 more in-flight > 2 is blocked");
    CHECK(r.tryReserveOpen("o3", "rb", false, 3500, 5) == OpenGate::Ok, "lot cap: the short side is uncapped");
    r.releaseReservation("o1");
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 2) == OpenGate::Ok, "lot cap: after release, 2 long is ok again");
  }

  // --- onOrderUpdate reconciles a reservation to the working remainder ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", true, 2);
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 2) == OpenGate::Ok, "reconcile: reserve 2");
    r.onOrderUpdate(1, 7, "o1", "rb", true, true, '1', 3500, 2, 1); // 1 filled, 1 still working
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::Ok,
          "reconcile: working remainder shrank to 1, freeing room for 1 more");
    r.onOrderUpdate(1, 7, "o1", "rb", true, true, '0', 3500, 2, 2); // terminal -> reservation gone
    CHECK(true, "reconcile: terminal status releases the reservation");
  }

  // --- a DIFFERENT session's working order counts against our caps (multi-terminal) ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", true, 2);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '3', 3500, 2, 0); // another terminal's working open
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "multi-terminal: another session's 2-lot working order consumes our cap");
  }

  // --- rebuildOpenReservations (authoritative resync from reqQryOrder) ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", true, 3);
    std::vector<OpenOrderInfo> working;
    OpenOrderInfo o;
    o.frontId = 1;
    o.sessionId = 7;
    o.orderRef = "5";
    o.instrumentId = "rb";
    o.isLong = true;
    o.vol = 2;
    o.price = 3500;
    working.push_back(o);
    r.rebuildOpenReservations(working);
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::Ok, "rebuild: 2 reserved + 1 = 3 at cap, ok");
    CHECK(r.tryReserveOpen("o3", "rb", true, 3500, 1) == OpenGate::VolumeLimit, "rebuild: a 4th lot over cap 3 is blocked");
  }

  // --- sanePositive / DBL_MAX guards (the round 11/14/17 bug fixes) ---
  {
    RiskEngine r;
    r.setMultiplier("rb", DMAX);          // sentinel multiplier -> must fall back to 1.0
    r.onTrade("rb", true, true, 3500, 1); // cost = 3500*1*1.0, NOT Inf
    CHECK(r.currentPositionCost() == 3500, "DBL_MAX multiplier falls back to 1.0 (cost not poisoned)");
  }
  {
    RiskEngine r;
    r.setMultiplier("rb", 10);
    r.onTrade("rb", true, true, DMAX, 1); // sentinel fill price -> rejected
    CHECK(r.currentPositionCost() == 0.0, "a DBL_MAX fill price is rejected (held cost unchanged)");
  }
  {
    RiskEngine r; // negative / non-finite seeds are rejected (never poison the cost)
    r.seedPosition("rb", true, -1, 100); // negative volume
    r.seedPosition("rb", true, 1, -5);   // negative cost
    CHECK(r.currentPositionCost() == 0.0, "seedPosition rejects a negative volume / cost");
  }
  {
    RiskEngine r; // inputs each pass sanePositive but their PRODUCT overflows -> per-fill backstop
    r.onTrade("rb", true, true, 1e299, 1e299); // 1e299 * 1e299 = +Inf
    CHECK(r.currentPositionCost() == 0.0, "onTrade rejects a fill whose cost product is non-finite");
  }
  {
    // RESERVATION-path sentinel guard (costPrice): a working open order with a
    // DBL_MAX limit price must reserve 0 cost (not Inf) so that when it goes
    // terminal the reconcile can't yield NaN and silently void every cost cap
    // book-wide. This is the round-17 hole; costPrice() closes it.
    RiskEngine r;
    RiskConfig c;
    c.maxPositionCost = 100000;
    r.configure(c);
    r.setSession(1, 7);
    r.setMultiplier("rb", 10);
    CHECK(r.tryReserveOpen("o1", "rb", true, DMAX, 1) == OpenGate::Ok,
          "reservation: a DBL_MAX limit price reserves 0 cost (not Inf), so it passes the cap");
    r.onOrderUpdate(1, 7, "o1", "rb", true, true, '0', DMAX, 1, 1); // terminal: reconcile must stay finite
    r.onTrade("rb", true, true, 9000, 2); // held 9000*2*10 = 180000, over the 100000 cap
    CHECK(r.tryReserveOpen("o2", "rb", true, 9000, 1) == OpenGate::CostLimit,
          "reservation: the cost cap STILL blocks after a DBL_MAX order (no NaN-void)");
  }
  {
    RiskEngine r;
    RiskConfig c;
    c.maxPositionCost = 100000;
    r.configure(c);
    r.seedPosition("rb", true, 1, DMAX); // absurd seed -> clamped finite
    r.seedPosition("rb", true, 1, DMAX); // two huge seeds: sum stays finite, never NaN
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 1) == OpenGate::CostLimit,
          "a huge seed trips the cost cap fail-safe (never NaN-voids it)");
  }

  // --- resetPositions clears positions but keeps multipliers + caps (config metadata) ---
  {
    RiskEngine r;
    r.setMultiplier("rb", 10);
    r.setMaxPositionVolume("rb", true, 2);
    r.onTrade("rb", true, true, 3500, 1);
    CHECK(r.currentPositionCost() > 0, "before reset: held cost is tracked");
    r.resetPositions();
    CHECK(r.currentPositionCost() == 0.0, "resetPositions() clears held cost");
    r.onTrade("rb", true, true, 3500, 1);
    CHECK(r.currentPositionCost() == 35000, "multiplier survives resetPositions (fill still costs x10)");
    CHECK(r.tryReserveOpen("o", "rb", true, 3500, 5) == OpenGate::VolumeLimit, "lot cap survives resetPositions");
  }

  // --- short-side mirrors (the long side is covered above) ---
  {
    RiskEngine r;
    r.setMultiplier("rb", 10);
    r.onTrade("rb", false, true, 3500, 2); // open SHORT 2 (sell-open) = 70000 held
    CHECK(r.currentPositionCost() == 70000, "short open accumulates held cost (70000)");
    r.onTrade("rb", true, false, 3400, 1); // partial close SHORT 1 (buy-close)
    CHECK(r.currentPositionCost() == 35000, "short partial close releases proportional cost (35000)");
  }
  {
    RiskEngine r;
    RiskConfig c;
    c.maxPositionCost = 50000;
    r.configure(c);
    r.seedPosition("rb", false, 2, 60000); // seed a SHORT position over the cap
    CHECK(r.tryReserveOpen("o", "rb", false, 1, 1) == OpenGate::CostLimit, "seeded short cost trips the global cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", false, 2); // SHORT lot cap
    CHECK(r.tryReserveOpen("s1", "rb", false, 3500, 2) == OpenGate::Ok, "short reserve 2 at cap, ok");
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 1) == OpenGate::VolumeLimit, "short over cap blocked");
    r.onOrderUpdate(1, 7, "s1", "rb", true, false, '1', 3500, 2, 1); // short working remainder -> 1
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 1) == OpenGate::Ok, "short reconcile frees room for 1 more");
    r.onOrderUpdate(1, 7, "s1", "rb", true, false, '0', 3500, 2, 2); // terminal
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", false, 3);
    std::vector<OpenOrderInfo> working;
    OpenOrderInfo a;
    a.frontId = 1; a.sessionId = 7; a.orderRef = "9"; a.instrumentId = "rb"; a.isLong = false; a.vol = 2; a.price = 3500;
    OpenOrderInfo z; // vol 0 -> skipped by the rebuild
    z.frontId = 1; z.sessionId = 7; z.orderRef = "x"; z.instrumentId = "rb"; z.isLong = false; z.vol = 0; z.price = 3500;
    working.push_back(a);
    working.push_back(z);
    r.rebuildOpenReservations(working);
    CHECK(r.tryReserveOpen("s3", "rb", false, 3500, 2) == OpenGate::VolumeLimit, "rebuild short: 2 reserved + 2 > 3 blocked (0-vol row skipped)");
  }

  // --- per-instrument cost cap counting an existing HELD position (instrCost) ---
  {
    RiskEngine r;
    r.setMultiplier("ag", 15);
    r.setMaxInstrumentCost("ag", 100000);
    r.onTrade("ag", true, true, 4000, 1); // held 4000*1*15 = 60000
    CHECK(r.tryReserveOpen("o", "ag", true, 4000, 1) == OpenGate::CostLimit,
          "per-instrument cap counts held + reserved (60000 + 60000 > 100000)");
  }

  // --- edge guards ---
  {
    RiskEngine r; // uncapped instrument: tryReserveOpen is Ok and reserves nothing
    CHECK(r.tryReserveOpen("o", "rb", true, 3500, 100) == OpenGate::Ok, "uncapped instrument: always ok, no reservation");
  }
  {
    RiskEngine r; // unconfigured -> rate limiter disabled
    CHECK(r.allowRate(), "rate limiter disabled (rate 0) always allows");
  }
  {
    RiskEngine r;
    CHECK(r.refPrice("rb") == 0.0, "refPrice is 0 with no snapshot attached");
    r.setMultiplier("rb", 0); // non-positive -> ignored (no-op)
    r.setMultiplier("rb", -1);
    CHECK(true, "setMultiplier ignores a non-positive multiplier");
  }
  {
    RiskEngine r;
    RiskConfig c;
    c.maxOrderVolume = 10;
    c.maxNotional = 1e9;
    r.configure(c);
    CHECK(!r.check("rb", 3500, 0, 0).ok, "check: volume <= 0 is blocked");
    CHECK(!r.check("rb", std::numeric_limits<double>::infinity(), 0, 1).ok, "check: a non-finite price is blocked");
  }
  {
    RiskEngine r;
    r.releaseReservation("nope"); // unknown ref -> no-op
    RiskConfig c;
    c.maxPositionCost = 1e6;
    r.configure(c);
    for (int i = 0; i < 5; i++)
      r.seedPosition("rb", true, 1, 1e308); // sum overflows -> clamped to max finite
    CHECK(r.tryReserveOpen("o", "rb", true, 1, 1) == OpenGate::CostLimit,
          "an overflowed cost sum stays finite and still trips the cap (never NaN-voids it)");
  }

  // --- onOrderUpdate (untracked tracking via hasCapLocked) / release / rebuild ---
  {
    RiskEngine r; // a non-opening order is ignored (no reservation side effects)
    r.onOrderUpdate(1, 1, "x", "rb", false, true, '3', 3500, 1, 0);
    CHECK(true, "onOrderUpdate ignores a non-opening order");
  }
  {
    // an untracked working order under the GLOBAL cost cap starts being tracked
    RiskEngine r;
    RiskConfig c;
    c.maxPositionCost = 100000;
    r.configure(c);
    r.setSession(1, 7);
    r.setMultiplier("rb", 10);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '3', 3500, 1, 0); // other terminal, working: reserves 35000
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 2) == OpenGate::CostLimit,
          "untracked order under the global cost cap consumes it (35000 + 70000 > 100000)");
  }
  {
    // an untracked working order under a PER-INSTRUMENT cost cap starts being tracked
    RiskEngine r;
    r.setSession(1, 7);
    r.setMultiplier("ag", 15);
    r.setMaxInstrumentCost("ag", 50000);
    r.onOrderUpdate(2, 9, "1", "ag", true, true, '3', 2000, 1, 0); // reserves 30000
    CHECK(r.tryReserveOpen("mine", "ag", true, 2000, 1) == OpenGate::CostLimit,
          "untracked order under the per-instrument cost cap consumes it (30000 + 30000 > 50000)");
  }
  {
    // an untracked working SHORT order under a per-side lot cap starts being tracked
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", false, 5);
    r.onOrderUpdate(2, 9, "1", "rb", true, false, '3', 3500, 3, 0); // reserves 3 short
    CHECK(r.tryReserveOpen("mine", "rb", false, 3500, 3) == OpenGate::VolumeLimit,
          "untracked short order consumes the short lot cap (3 + 3 > 5)");
  }
  {
    RiskEngine r; // no cap at all -> an untracked order is not tracked (hasCapLocked false)
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '3', 3500, 1, 0);
    CHECK(true, "untracked order with no cap in force is not tracked");
  }
  {
    RiskEngine r; // negative working remainder (volTraded > volTotal) clamps to 0
    r.setMaxPositionVolume("rb", true, 5);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '3', 3500, 1, 2); // remainder volTotal1 - volTraded2 = -1 -> clamped 0
    CHECK(true, "onOrderUpdate clamps a negative working remainder to 0");
  }
  {
    RiskEngine r; // releaseReservation on a SHORT reservation
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", false, 5);
    r.tryReserveOpen("s", "rb", false, 3500, 2); // reserve 2 short
    r.releaseReservation("s");
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 5) == OpenGate::Ok, "releaseReservation frees a short reservation");
  }
  {
    // rebuild clears prior in-flight reservations before re-reserving, and dedupes
    // duplicate (front:session:ref) rows.
    RiskEngine r;
    r.setSession(1, 7);
    r.setMaxPositionVolume("rb", true, 5);
    r.tryReserveOpen("o", "rb", true, 3500, 4); // a prior reservation (4 long pend) to be cleared
    std::vector<OpenOrderInfo> working;
    OpenOrderInfo a;
    a.frontId = 1; a.sessionId = 7; a.orderRef = "7"; a.instrumentId = "rb"; a.isLong = true; a.vol = 2; a.price = 3500;
    OpenOrderInfo dup = a; // same (front:session:ref) -> deduped
    working.push_back(a);
    working.push_back(dup);
    r.rebuildOpenReservations(working);
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 3) == OpenGate::Ok,
          "rebuild cleared the prior reservation + deduped the row (2 reserved + 3 = 5 at cap, ok)");
    CHECK(r.tryReserveOpen("o3", "rb", true, 3500, 1) == OpenGate::VolumeLimit, "rebuild: a further lot over cap is blocked");
  }

  REPORT("RISK-CPP TEST");
}
