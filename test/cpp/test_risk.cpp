/*
 * test_risk.cpp - standalone unit tests for RiskEngine (src/native/risk.cc).
 *
 * Links risk.cc DIRECTLY - no N-API, no addon, no test-only hooks in the shipped
 * library. Exercises the safety-critical pre-trade controls (the same logic the
 * addon enforces on every order) plus the DBL_MAX/sanePositive guards, in C++
 * isolation. Build + run via `npm run test:cpp`.
 */
#include "risk.h"

#include <cmath>
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
  uint64_t age = 0;
  bool quote(const std::string &id, double &price,
             uint64_t &ageOut) const override {
    auto it = px.find(id);
    if (it == px.end()) return false;
    price = it->second;
    ageOut = age;
    return true;
  }
};

static void setReference(RiskEngine &r, const std::string &instrument,
                         double price = 1.0, uint64_t age = 0) {
  auto snap = std::make_shared<MockSnap>();
  snap->px[instrument] = price;
  snap->age = age;
  r.setMdSnapshot(snap);
}

static void setFuture(RiskEngine &r, const std::string &instrument,
                      double multiplier) {
  r.setInstrument(instrument, multiplier, '1');
}

static void setSimpleMargin(RiskEngine &r, const std::string &instrument,
                            double rate) {
  setReference(r, instrument);
  r.setMarginSchedule(instrument, '1', rate, 0.0, rate, 0.0, false);
}

static bool setPositionLimit(RiskEngine &r, const std::string &instrument,
                             bool isLong, double limit) {
  return r.setInstrumentPositionLimits({InstrumentPositionLimitUpdate{
      instrument, isLong, isLong ? limit : 0.0, !isLong,
      isLong ? 0.0 : limit}});
}

static bool setMarginLimit(RiskEngine &r, const std::string &instrument,
                           double limit) {
  return r.setInstrumentMarginLimits(
      {InstrumentMarginLimitUpdate{instrument, limit}});
}

static bool seedHeld(RiskEngine &r, const std::string &instrument,
                     bool isLong, double volume, double margin,
                     char hedgeFlag = '1') {
  return r.replacePositionsIfRevision(
      r.revision(), {PositionSeed{instrument, isLong, volume, margin,
                                  hedgeFlag}});
}

static void recordTrade(RiskEngine &r, const std::string &key,
                        const std::string &instrument, bool isBuy,
                        bool isOpen, double price, double volume,
                        char hedgeFlag = '1') {
  r.onTrade(key, "", "", instrument, isBuy, isOpen, hedgeFlag, price,
            volume);
}

static void recordTrade(RiskEngine &r, const std::string &instrument,
                        bool isBuy, bool isOpen, double price, double volume) {
  static uint64_t sequence = 0;
  recordTrade(r, "20990101|local|" + std::to_string(++sequence), instrument,
              isBuy, isOpen, price, volume);
}

int main() {
  // --- check(): per-order limits ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrderVolume = 10;
    r.configure(c);
    CHECK(r.check("rb", 3500, 0, 10).ok, "maxOrderVolume: at the cap is allowed");
    CHECK(!r.check("rb", 3500, 0, 11).ok, "maxOrderVolume: over the cap is blocked");
    CHECK(r.lastBlockReason() != nullptr, "lastBlockReason is set after a block");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = 1000000;
    r.configure(c);
    setFuture(r, "rb", 10);
    CHECK(!r.check("rb", 3500, 3500, 30).ok, "maxNotional: 3500*30*10=1.05M over 1M is blocked (multiplier-aware)");
    CHECK(r.check("rb", 3500, 3500, 28).ok, "maxNotional: 3500*28*10=0.98M under 1M is allowed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = 1000000;
    r.configure(c);
    setFuture(r, "rb", 10);
    CHECK(r.check("rb", 3500, 3500, 1).ok,
          "known multiplier permits a capped notional check");
    r.invalidateInstrument("rb");
    CHECK(!r.check("rb", 3500, 3500, 1).ok,
          "a metadata refresh invalidates stale multiplier data before query results arrive");
    setFuture(r, "rb", 10);
    r.setInstrument("rb", DMAX, '1');
    CHECK(!r.check("rb", 3500, 3500, 1).ok,
          "an invalid multiplier response clears the old value instead of retaining it");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = 10000;
    r.configure(c);
    setFuture(r, "rb", 10);
    CHECK(!r.check("rb", 1, 0, 1).ok,
          "maxNotional fails closed without a fresh reference price");
    CHECK(!r.check("rb", 1, 3500, 1).ok,
          "a low sell/open limit cannot understate maxNotional versus the market");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxPriceDeviation = 0.02;
    r.configure(c);
    CHECK(!r.check("rb", 3600, 3500, 1).ok, "maxPriceDeviation: 2.86% over 2% is blocked");
    CHECK(r.check("rb", 3550, 3500, 1).ok, "maxPriceDeviation: 1.43% under 2% is allowed");
    CHECK(!r.check("rb", 9999, 0, 1).ok,
          "maxPriceDeviation: a missing reference fails closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrderVolume = 10;
    r.configure(c);
    r.reduceOnly();
    CHECK(!r.check("rb", 3500, 0, 1).ok,
          "reduce-only blocks risk-increasing orders");
    CHECK(r.mode() == RiskMode::ReduceOnly,
          "mode() reports REDUCE_ONLY");
    r.resume();
    CHECK(r.check("rb", 3500, 0, 1).ok, "kill-switch: resume allows orders again");
  }
  {
    RiskEngine r; // all-zero config = every control disabled
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    CHECK(r.check("rb", 1e9, 0, 1000000).ok, "disabled config: nothing is blocked");
  }

  // --- rate limiter (token bucket) ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
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
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    CHECK(r.refPrice("rb") == 0.0, "no snapshot attached -> refPrice 0 (enabled guard will block)");
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

  // --- concrete-account onTrade: Current-match accumulates, Foreign is dropped ---
  // Production always passes a concrete broker/investor from the CTP struct, so
  // accountScopeLocked takes the strict comparison branch, not the standalone
  // short-circuit the empty-account setSession(1,7) blocks exercise.
  {
    RiskEngine r;
    r.setSession(1, 7, "9999", "u"); // concrete confirmed account
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    r.onTrade("20990101|9999|u|SHFE|rb|trade|t1", "1", "SHFE|sys1", "rb", true, true, '1', 3500, 2, "9999", "u");
    CHECK(r.accountMargin() == 70000, "concrete account: a matching-account fill accumulates held margin");
    r.onTrade("20990101|8888|x|SHFE|rb|trade|t2", "2", "SHFE|sys2", "rb", true, true, '1', 3500, 2, "8888", "x");
    CHECK(r.accountMargin() == 70000, "concrete account: a foreign-account fill is dropped, not double-counted");
  }

  // --- held margin (onTrade) + global maxAccountMargin cap ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    recordTrade(r, "rb", true, true, 3500, 2);
    CHECK(r.accountMargin() == 70000, "onTrade(open) accumulates held margin (70000)");
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 1) == OpenGate::CostLimit,
          "global cost cap: held 70000 + 35000 reserved > 100000 is blocked");
    recordTrade(r, "rb", false, false, 3600, 2);
    CHECK(r.accountMargin() < 1.0, "onTrade(close) releases the held margin (~0)");
  }

  // --- margin rate: tracked cost becomes real MARGIN = notional * marginRate ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0.1);
    recordTrade(r, "rb", true, true, 3500, 2);
    CHECK(r.accountMargin() == 7000, "margin schedule scales held margin to 7000");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setReference(r, "rb", 3500);
    r.setMarginSchedule("rb", '1', 0.20, 0.0, 0.20, 0.0, false);
    r.setMarginSchedule("rb", '3', 0.05, 0.0, 0.05, 0.0, false);
    recordTrade(r, "20990101|local|spec-open", "rb", true, true, 3500, 1,
                '1');
    recordTrade(r, "20990101|local|hedge-open", "rb", true, true, 3500, 1,
                '3');
    CHECK(r.accountMargin() == 8750,
          "different hedge flags retain separate held-margin buckets");
    recordTrade(r, "20990101|local|hedge-close", "rb", false, false, 3500, 1,
                '3');
    CHECK(r.accountMargin() == 7000,
          "closing a low-margin hedge bucket preserves high-margin speculation cost");
  }
  {
    RiskEngine r; // uncapped observability keeps a best-effort notional value
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    recordTrade(r, "rb", true, true, 3500, 2);
    CHECK(r.accountMargin() == 70000, "uncapped missing schedule keeps best-effort observability");
  }
  {
    RiskEngine r; // global maxAccountMargin cap (in margin units)
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 10000;
    r.configure(c);
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0.1);
    recordTrade(r, "rb", true, true, 3500, 2);
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 1) == OpenGate::CostLimit,
          "maxAccountMargin: held 7000 + 3500 reserved > 10000 is blocked");
    CHECK(r.tryReserveOpen("o2", "rb", true, 2000, 1) == OpenGate::Ok,
          "maxAccountMargin: held 7000 + 2000 reserved = 9000 is allowed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 1000;
    r.configure(c);
    setFuture(r, "rb", 10);
    setReference(r, "rb", 3500);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    CHECK(r.tryReserveOpen("low-sell", "rb", false, 1, 1) ==
              OpenGate::CostLimit,
          "a low sell/open limit is valued at the higher fresh market reference");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    c.maxReferenceAgeMs = 100;
    r.configure(c);
    setFuture(r, "rb", 10);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    CHECK(r.tryReserveOpen("missing-ref", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a monetary cap fails closed when the reference price is missing");
    setReference(r, "rb", 3500, 101);
    CHECK(r.tryReserveOpen("stale-ref", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a monetary cap fails closed when the reference price is stale");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    // Cap 1500 so the RESTING order's revaluation is what trips it, not the new
    // order's own cost: after px->1000 the new order alone costs 1000 (< 1500,
    // admissible); only if the resting short is also re-valued 100->1000 does the
    // total (2000) exceed 1500. Without tryReserveOpen's refresh the resting
    // order stays at 100 and the total (1100) stays Ok — so this CHECK flips
    // exactly on the admission-time revaluation it names.
    c.maxAccountMargin = 1500;
    r.configure(c);
    setFuture(r, "rb", 10);
    auto snap = std::make_shared<MockSnap>();
    snap->px["rb"] = 100;
    r.setMdSnapshot(snap);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    CHECK(r.tryReserveOpen("resting-sell", "rb", false, 100, 1) ==
              OpenGate::Ok,
          "working-order revaluation setup admits the initial exposure");
    snap->px["rb"] = 1000;
    CHECK(r.tryReserveOpen("after-market-rise", "rb", true, 100, 1) ==
              OpenGate::CostLimit,
          "existing working orders are revalued before the next monetary admission");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0.1);
    recordTrade(r, "rb", true, true, 3500, 1);
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    r.invalidateMarginSchedules("rb", '1');
    setSimpleMargin(r, "rb", 0.1);
    CHECK(r.tryReserveOpen("stale-held-margin", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a margin refresh makes old estimated held cost unknown until position sync");
    CHECK(r.replacePositionsIfRevision(
              r.revision(), {PositionSeed{"rb", true, 1, 3500}}),
          "authoritative position margin reconciles a schedule refresh");
    CHECK(r.tryReserveOpen("fresh-held-margin", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "monetary opens resume after authoritative held-margin reconciliation");
  }
  {
    RiskEngine r; // invalid schedules are ignored; uncapped accounting stays observable
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0);
    setSimpleMargin(r, "rb", -0.5);
    setSimpleMargin(r, "rb", DMAX);
    recordTrade(r, "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 35000, "invalid margin schedule does not poison uncapped accounting");
  }
  {
    RiskEngine r; // marginRate is static metadata: survives resetPositions (like multipliers)
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0.1);
    recordTrade(r, "rb", true, true, 3500, 1);
    r.resetPositions();
    recordTrade(r, "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 3500, "margin schedule survives resetPositions");
  }

  // --- per-instrument cost cap ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "ag", 15);
    setSimpleMargin(r, "ag", 1.0);
    setMarginLimit(r, "ag", 50000);
    CHECK(r.tryReserveOpen("o1", "ag", true, 5000, 1) == OpenGate::CostLimit,
          "per-instrument cost cap: 5000*1*15=75000 > 50000 is blocked");
    CHECK(r.tryReserveOpen("o2", "ag", true, 3000, 1) == OpenGate::Ok,
          "per-instrument cost cap: 3000*1*15=45000 < 50000 is allowed");
  }

  // --- per-side lot cap + in-flight reservation ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 2); // long capped at 2 lots
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 2) == OpenGate::Ok, "lot cap: reserve 2 long = at cap, ok");
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "lot cap: 2 reserved + 1 more in-flight > 2 is blocked");
    CHECK(r.tryReserveOpen("o3", "rb", false, 3500, 5) == OpenGate::Ok, "lot cap: the short side is uncapped");
    CHECK(r.bindInsertRequest(101, "o1"),
          "lot cap: reservation binds to its exact request id");
    r.rejectInsertByRequest(101, "o1");
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 2) == OpenGate::Ok, "lot cap: after release, 2 long is ok again");
  }

  // --- Order/Trade callback order cannot create a pending->held gap ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 2) == OpenGate::Ok, "reconcile: reserve 2");
    r.onOrderUpdate(1, 7, "o1", "rb", true, true, '1', '1', 3500, 2, 1,
                    "20260710|SHFE|rb|sys-o1");
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "order-before-trade keeps the reported fill reserved (no undercount window)");
    r.onTrade("20260710|SHFE|gap1", "o1",
              "20260710|SHFE|rb|sys-o1", "rb", true, true, '1', 3500, 1);
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "trade atomically transfers one lot pending->held; total commitment stays 2");
    r.onOrderUpdate(1, 7, "o1", "rb", true, true, '1', '2', 3500, 2, 1,
                    "20260710|SHFE|rb|sys-o1");
    CHECK(r.tryReserveOpen("o2", "rb", true, 3500, 1) == OpenGate::Ok,
          "partial-not-queueing releases only the canceled remainder");
  }

  // --- a DIFFERENT session's working order counts against our caps (multi-terminal) ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 2, 0);
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "multi-terminal: another session's 2-lot working order consumes our cap");
  }

  // --- rebuildOpenReservations (authoritative resync from reqQryOrder) ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 3);
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
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", DMAX); // sentinel multiplier remains unknown
    recordTrade(r, "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 3500,
          "DBL_MAX multiplier stays unknown without poisoning uncapped accounting");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    recordTrade(r, "rb", true, true, DMAX, 1);
    CHECK(r.accountMargin() == 0.0, "a DBL_MAX fill price is rejected");
  }
  {
    RiskEngine r; // negative / non-finite seeds are rejected (never poison the cost)
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    seedHeld(r, "rb", true, -1, 100); // negative volume
    seedHeld(r, "rb", true, 1, -5);   // negative cost
    CHECK(r.accountMargin() == 0.0, "position replacement rejects a negative volume / margin");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0.1);
    seedHeld(r, "rb", true, 1, 0);
    CHECK(r.tryReserveOpen("zero-seed-margin", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a positive held position with zero reported margin fails closed");
  }
  {
    RiskEngine r; // valid lot count, but price*multiplier overflows -> per-fill backstop
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 1e299);
    recordTrade(r, "rb", true, true, 1e299, 1);
    CHECK(std::isfinite(r.accountMargin()) && r.accountMargin() > 0.0,
          "onTrade pins an overflowing fill to a finite fail-safe cost");
  }
  {
    // An unusable price under a monetary cap must fail closed. Reserving zero
    // would let a market/zero/sentinel-priced opening order bypass maxAccountMargin.
    RiskEngine r;
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    r.setSession(1, 7);
    setFuture(r, "rb", 10);
    // Valid reference + schedule so a usable price would be admissible; only the
    // DBL_MAX limit price (rejected by sanePositive inside priceKnown, though it
    // passes the structural isfinite gate) is left to force fail-closed here.
    setReference(r, "rb", 3500);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    CHECK(r.tryReserveOpen("o1", "rb", true, DMAX, 1) ==
              OpenGate::MetadataUnavailable,
          "reservation: a DBL_MAX limit price fails closed under a cost cap");
    r.onOrderUpdate(1, 7, "o1", "rb", true, true, '1', '0', DMAX, 1, 1);
    recordTrade(r, "rb", true, true, 9000, 2);
    CHECK(r.tryReserveOpen("o2", "rb", true, 9000, 1) ==
              OpenGate::MetadataUnavailable,
          "reservation: a DBL_MAX order update leaves the ledger fail-closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    seedHeld(r, "rb", true, 1, DMAX); // absurd seed -> clamped finite
    seedHeld(r, "rb", true, 1, DMAX); // repeated huge replacement stays finite
    CHECK(r.tryReserveOpen("o1", "rb", true, 3500, 1) == OpenGate::CostLimit,
          "a huge seed trips the cost cap fail-safe (never NaN-voids it)");
  }

  // --- resetPositions clears positions but keeps multipliers + caps (config metadata) ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setPositionLimit(r, "rb", true, 2);
    recordTrade(r, "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() > 0, "before reset: held margin is tracked");
    r.resetPositions();
    CHECK(r.accountMargin() == 0.0, "resetPositions() clears held margin");
    CHECK(r.tryReserveOpen("while-reset", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "resetPositions() keeps opens fail-closed until authoritative reseed");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}),
          "authoritative flat snapshot clears reset uncertainty");
    recordTrade(r, "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 35000, "instrument metadata survives resetPositions");
    CHECK(r.tryReserveOpen("o", "rb", true, 3500, 5) == OpenGate::VolumeLimit, "lot cap survives resetPositions");
  }

  // --- short-side mirrors (the long side is covered above) ---
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    recordTrade(r, "rb", false, true, 3500, 2);
    CHECK(r.accountMargin() == 70000, "short open accumulates held margin (70000)");
    recordTrade(r, "rb", true, false, 3400, 1);
    CHECK(r.accountMargin() == 35000, "short close releases proportional margin");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    seedHeld(r, "rb", true, 1, 35000);
    recordTrade(r, "rb", false, false, 3500, 2); // a close (here over-held: 2 > 1)
    // Every close marks the position ledger uncertain (risk.cc close branch: the
    // realtime stream and a position query share no sequence watermark, so a
    // possible double-release must never reopen a cap) — so opening inserts fail
    // closed until an authoritative position sync. (An over-close is one trigger,
    // but the guard is unconditional; the specific over-held nuance is not
    // separately observable through this gate.)
    CHECK(r.tryReserveOpen("after-close", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a close marks the position ledger uncertain so opening inserts fail closed until authoritative resync");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 50000;
    r.configure(c);
    seedHeld(r, "rb", false, 2, 60000); // seed a SHORT position over the cap
    CHECK(r.tryReserveOpen("o", "rb", false, 1, 1) == OpenGate::CostLimit, "seeded short cost trips the global cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", false, 2); // SHORT lot cap
    CHECK(r.tryReserveOpen("s1", "rb", false, 3500, 2) == OpenGate::Ok, "short reserve 2 at cap, ok");
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 1) == OpenGate::VolumeLimit, "short over cap blocked");
    r.onOrderUpdate(1, 7, "s1", "rb", true, false, '1', '1', 3500, 2, 1,
                    "20260710|SHFE|rb|sys-s1");
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 1) == OpenGate::VolumeLimit,
          "short order-before-trade retains the in-flight fill reservation");
    r.onTrade("20260710|SHFE|sgap", "s1",
              "20260710|SHFE|rb|sys-s1", "rb", false, true, '1', 3500, 1);
    r.onOrderUpdate(1, 7, "s1", "rb", true, false, '1', '2', 3500, 2, 1,
                    "20260710|SHFE|rb|sys-s1");
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 1) == OpenGate::Ok,
          "short partial cancel releases only its unfilled remainder");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", false, 3);
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
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "ag", 15);
    setSimpleMargin(r, "ag", 1.0);
    setMarginLimit(r, "ag", 100000);
    recordTrade(r, "ag", true, true, 4000, 1);
    CHECK(r.tryReserveOpen("o", "ag", true, 4000, 1) == OpenGate::CostLimit,
          "per-instrument cap counts held + reserved (60000 + 60000 > 100000)");
  }

  // --- edge guards ---
  {
    RiskEngine r; // uncapped instrument is accepted but still tracked for later caps
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    CHECK(r.tryReserveOpen("o", "rb", true, 3500, 100) == OpenGate::Ok, "uncapped instrument: accepted and tracked for later cap activation");
  }
  {
    RiskEngine r; // unconfigured -> rate limiter disabled
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    CHECK(r.allowRate(), "rate limiter disabled (rate 0) always allows");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrdersPerSec = 0.5;
    r.configure(c);
    CHECK(r.allowRate(),
          "fractional order rate gets a one-token minimum burst (never starves forever)");
    CHECK(!r.allowRate(), "fractional order rate still throttles the next immediate send");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxCloseOrdersPerSec = 1;
    c.closeOrderBurst = 1;
    r.configure(c);
    CHECK(r.allowCloseRate() && !r.allowCloseRate(),
          "closing orders use an independent optional token bucket");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    CHECK(r.refPrice("rb") == 0.0, "refPrice is 0 with no snapshot attached");
    RiskLimits c;
    c.maxNotional = 100000;
    r.configure(c);
    setFuture(r, "rb", 0);  // non-positive multipliers are ignored: the value
    setFuture(r, "rb", -1); // stays UNKNOWN (not stored as 0/-1, which would
                            // zero the notional and bypass the cap).
    CHECK(!r.check("rb", 3500, 3500, 1).ok,
          "setInstrument ignores a non-positive multiplier (stays unknown -> capped notional fails closed)");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrderVolume = 10;
    c.maxNotional = 1e9;
    r.configure(c);
    CHECK(!r.check("rb", 3500, 0, 0).ok, "check: volume <= 0 is blocked");
    CHECK(!r.check("rb", std::numeric_limits<double>::infinity(), 0, 1).ok, "check: a non-finite price is blocked");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.rejectInsertUnscoped("nope"); // no matching live ref -> no-op
    RiskLimits c;
    c.maxAccountMargin = 1e6;
    r.configure(c);
    for (int i = 0; i < 5; i++)
      seedHeld(r, "rb", true, 1, 1e308); // huge authoritative margin stays fail-closed
    CHECK(r.tryReserveOpen("o", "rb", true, 1, 1) == OpenGate::CostLimit,
          "an overflowed cost sum stays finite and still trips the cap (never NaN-voids it)");
  }

  // --- onOrderUpdate tracking / release / rebuild ---
  {
    RiskEngine r; // a non-opening order must have NO reservation side effects
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 1); // long capped at 1 lot
    r.onOrderUpdate(1, 1, "x", "rb", false, true, '1', '3', 3500, 1, 0); // isOpen=false
    // If the `if (!isOpen) return;` guard were removed, this non-opening update
    // would consume the 1-lot long cap and the reserve below would trip
    // VolumeLimit. It must stay Ok — proving the order was genuinely ignored.
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 1) == OpenGate::Ok,
          "onOrderUpdate ignores a non-opening order (no reservation side effects)");
  }
  {
    // an untracked working order under the GLOBAL cost cap starts being tracked
    RiskEngine r;
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    r.setSession(1, 7);
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 1, 0);
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 2) == OpenGate::CostLimit,
          "untracked order under the global cost cap consumes it (35000 + 70000 > 100000)");
  }
  {
    // an untracked working order under a PER-INSTRUMENT cost cap starts being tracked
    RiskEngine r;
    r.setSession(1, 7);
    setFuture(r, "ag", 15);
    setSimpleMargin(r, "ag", 1.0);
    setMarginLimit(r, "ag", 50000);
    r.onOrderUpdate(2, 9, "1", "ag", true, true, '1', '3', 2000, 1, 0);
    CHECK(r.tryReserveOpen("mine", "ag", true, 2000, 1) == OpenGate::CostLimit,
          "untracked order under the per-instrument cost cap consumes it (30000 + 30000 > 50000)");
  }
  {
    // an untracked working SHORT order under a per-side lot cap starts being tracked
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", false, 5);
    r.onOrderUpdate(2, 9, "1", "rb", true, false, '1', '3', 3500, 3, 0);
    CHECK(r.tryReserveOpen("mine", "rb", false, 3500, 3) == OpenGate::VolumeLimit,
          "untracked short order consumes the short lot cap (3 + 3 > 5)");
  }
  {
    RiskEngine r; // no cap at all -> still retained for later cap activation
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 1, 0);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "untracked order with no cap in force is retained for later caps");
  }
  {
    RiskEngine r;
    r.setSession(2, 9);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("1", "rb", true, 3500, 1) == OpenGate::Ok,
          "malformed-order setup reserves one lot");
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 1, 2);
    CHECK(r.tryReserveOpen("2", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "volTraded > volTotal makes both ledgers fail closed");
  }
  {
    RiskEngine r; // exact request rejection on a SHORT reservation
    r.setSession(1, 7);
    setPositionLimit(r, "rb", false, 5);
    r.tryReserveOpen("s", "rb", false, 3500, 2); // reserve 2 short
    CHECK(r.bindInsertRequest(102, "s"),
          "short reservation binds before its insert");
    r.rejectInsertByRequest(102, "s");
    CHECK(r.tryReserveOpen("s2", "rb", false, 3500, 5) == OpenGate::Ok, "exact rejection frees a short reservation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    r.onOrderUpdate(2, 9, "shared", "rb", true, true, '1', '3', 3500,
                    1, 0, "20260710|SHFE|rb|external");
    r.rejectInsertUnscoped("shared");
    CHECK(r.tryReserveOpen("after-ambiguous-reject", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a rejection without front/session cannot release another terminal's same-ref reservation");
  }
  {
    // rebuild clears prior in-flight reservations before re-reserving, and dedupes
    // duplicate (front:session:ref) rows.
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 5);
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
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 1);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    setReference(r, "rb", 100);
    setMarginLimit(r, "rb", 15);
    OpenOrderInfo working;
    working.frontId = 1;
    working.sessionId = 7;
    working.orderRef = "priced-below-market";
    working.instrumentId = "rb";
    working.vol = 1;
    working.price = 1;
    r.rebuildOpenReservations({working});
    CHECK(r.tryReserveOpen("after-priced-snapshot", "rb", true, 100, 1) ==
              OpenGate::CostLimit,
          "rebuild values a working order at max(limit, fresh reference)");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 1);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    setReference(r, "rb", 100);
    setMarginLimit(r, "rb", 1000);
    OpenOrderInfo working;
    working.frontId = 1;
    working.sessionId = 7;
    working.orderRef = "zero-price";
    working.instrumentId = "rb";
    working.vol = 1;
    working.price = 0;
    r.rebuildOpenReservations({working});
    CHECK(r.tryReserveOpen("after-zero-price", "rb", true, 100, 1) ==
              OpenGate::MetadataUnavailable,
          "zero-price snapshot reservation stays unknown under a monetary cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 1);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    setMarginLimit(r, "rb", 1000);
    OpenOrderInfo working;
    working.frontId = 1;
    working.sessionId = 7;
    working.orderRef = "missing-reference";
    working.instrumentId = "rb";
    working.vol = 1;
    working.price = 100;
    r.rebuildOpenReservations({working});
    CHECK(r.tryReserveOpen("after-missing-reference", "rb", true, 100, 1) ==
              OpenGate::MetadataUnavailable,
          "snapshot reservation without a fresh reference fails monetary gates closed");
  }

  // --- production regression cases: monotonic/idempotent ledger + fail-closed metadata ---
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 5);
    CHECK(r.tryReserveOpen("1", "rb", true, 3500, 2) == OpenGate::Ok,
          "duplicate ref: first insert reserves normally");
    CHECK(r.tryReserveOpen("1", "rb", true, 3500, 2) ==
              OpenGate::DuplicateOrderRef,
          "duplicate ref: a second insert cannot overwrite the original ledger entry");
    CHECK(r.tryReserveOpen("2", "rb", true, 3500, 3) == OpenGate::Ok,
          "duplicate ref: no phantom reservation is created");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("retry", "rb", true, 3500, 1) == OpenGate::Ok,
          "pre-send rollback: first reservation succeeds");
    CHECK(r.bindInsertRequest(103, "retry"),
          "pre-send rollback: exact request binding succeeds");
    r.rollbackInsertRequest(103, "retry");
    CHECK(r.tryReserveOpen("retry", "rb", true, 3500, 1) == OpenGate::Ok,
          "pre-send rollback: rate/send failure may retry the same OrderRef");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("same-ref", "rb", true, 3500, 1) == OpenGate::Ok &&
              r.bindInsertRequest(201, "same-ref"),
          "session A binds its reservation to request 201");
    r.invalidateSession();
    r.setSession(2, 8, "9999", "investor");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}) &&
              r.rebuildOpenReservationsIfRevision(r.revision(), {}),
          "session B starts from authoritative empty ledgers");
    CHECK(r.tryReserveOpen("same-ref", "rb", true, 3500, 1) == OpenGate::Ok &&
              r.bindInsertRequest(202, "same-ref"),
          "session B may independently reuse the same OrderRef");
    // The rejection carries the confirmed account, as the SPI now passes from the
    // echoed InputOrder (without it, a concrete-account session would treat every
    // insert rejection as Ambiguous and leak the reservation). Session A's
    // reservation was authoritatively evicted by session B's rebuild, so its late
    // rejection is a clean no-op and never releases session B's live reservation.
    r.rejectInsertByRequest(201, "same-ref", "9999", "investor");
    CHECK(r.tryReserveOpen("after-old-reject", "rb", true, 3500, 1) ==
              OpenGate::VolumeLimit,
          "late session-A rejection cannot release session-B reservation");
    r.rejectInsertByRequest(202, "same-ref", "9999", "investor");
    CHECK(r.tryReserveOpen("after-current-reject", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "the correctly correlated session-B rejection releases only B");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("null-row", "rb", true, 3500, 1) == OpenGate::Ok &&
              r.bindInsertRequest(205, "null-row"),
          "null response-row setup has an exact request binding");
    r.rejectInsertByRequest(205, "");
    CHECK(r.tryReserveOpen("after-null-row", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "request id alone safely correlates a rejection with a null row");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("unknown-id", "rb", true, 3500, 1) == OpenGate::Ok &&
              r.bindInsertRequest(206, "unknown-id"),
          "unknown-response setup has a live exact binding");
    r.rejectInsertByRequest(999, "unknown-id");
    CHECK(r.tryReserveOpen("after-unknown-id", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "unknown or evicted request id fails closed without releasing exposure");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("unscoped", "rb", true, 3500, 1) == OpenGate::Ok &&
              r.bindInsertRequest(203, "unscoped"),
          "unscoped rejection setup has one bound reservation");
    r.rejectInsertUnscoped("unscoped");
    CHECK(r.tryReserveOpen("after-unscoped", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "OnErrRtn without request id retains exposure and fails closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("observed", "rb", true, 3500, 1) == OpenGate::Ok &&
              r.bindInsertRequest(204, "observed"),
          "observed-order setup has one bound reservation");
    r.onOrderUpdate(1, 7, "observed", "rb", true, true, '1', '3', 3500,
                    1, 0, "20260710|SHFE|rb|observed");
    r.rejectInsertByRequest(204, "observed");
    CHECK(r.tryReserveOpen("after-contradiction", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "rejection after a working OnRtnOrder never releases accepted exposure");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    const std::string sys = "20260710|SHFE|rb|old";
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 2, 0, sys);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 2, 0, sys);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 2, 0, sys);
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 2) == OpenGate::Ok,
          "terminal order ignores a stale replay with the same exchange identity");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 3);
    const std::string oldSys = "20260710|SHFE|rb|old";
    const std::string newSys = "20260711|SHFE|rb|new";
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 2, 0,
                    oldSys);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 2, 0,
                    oldSys);
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 2, 0,
                    newSys);
    CHECK(r.tryReserveOpen("ambiguous-reuse", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a terminal key reused by a different exchange order fails closed");
    OpenOrderInfo current;
    current.frontId = 2;
    current.sessionId = 9;
    current.orderRef = "1";
    current.orderSysKey = newSys;
    current.instrumentId = "rb";
    current.isLong = true;
    current.vol = 2;
    current.price = 3500;
    CHECK(r.rebuildOpenReservationsIfRevision(r.revision(), {current}),
          "authoritative order sync supersedes a reused terminal tombstone");
    CHECK(r.tryReserveOpen("after-reuse-sync", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "reused working order is counted after authoritative reconciliation");
    CHECK(r.tryReserveOpen("reuse-cap", "rb", true, 3500, 1) ==
              OpenGate::VolumeLimit,
          "reused working order cannot be omitted from the position cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 10);
    const std::string oldSys = "20260710|SHFE|rb|old-terminal";
    const std::string newSys = "20260711|SHFE|rb|new-terminal";
    r.onOrderUpdate(2, 9, "terminal-reuse", "rb", true, true, '1', '5',
                    3500, 2, 0, oldSys);
    const uint64_t before = r.revision();
    r.onOrderUpdate(2, 9, "terminal-reuse", "rb", true, true, '1', '5',
                    3500, 2, 1, newSys);
    CHECK(r.revision() > before &&
              r.tryReserveOpen("blocked-terminal-reuse", "rb", true, 3500,
                               1) == OpenGate::MetadataUnavailable,
          "a different terminal exchange identity cannot masquerade as a duplicate terminal report");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    const std::string sys = "20260710|SHFE|rb|terminal";
    r.onOrderUpdate(2, 9, "same", "rb", true, true, '1', '3', 3500, 2, 0,
                    sys);
    r.onOrderUpdate(2, 9, "same", "rb", true, true, '1', '5', 3500, 2, 0,
                    sys);
    OpenOrderInfo stale;
    stale.frontId = 2;
    stale.sessionId = 9;
    stale.orderRef = "same";
    stale.orderSysKey = sys;
    stale.instrumentId = "rb";
    stale.isLong = true;
    stale.vol = 2;
    stale.price = 3500;
    CHECK(r.rebuildOpenReservationsIfRevision(r.revision(), {stale}),
          "authoritative sync recognizes a stale row for the same terminal exchange order");
    CHECK(r.tryReserveOpen("after-stale-snapshot", "rb", true, 3500, 2) ==
              OpenGate::Ok,
          "a stale snapshot row cannot resurrect terminal exposure");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    r.onOrderUpdate(2, 9, "ambiguous", "rb", true, true, '1', '3', 3500, 2,
                    0);
    r.onOrderUpdate(2, 9, "ambiguous", "rb", true, true, '1', '5', 3500, 2,
                    0);
    OpenOrderInfo ambiguous;
    ambiguous.frontId = 2;
    ambiguous.sessionId = 9;
    ambiguous.orderRef = "ambiguous";
    ambiguous.instrumentId = "rb";
    ambiguous.isLong = true;
    ambiguous.vol = 2;
    ambiguous.price = 3500;
    CHECK(!r.rebuildOpenReservationsIfRevision(r.revision(), {ambiguous}),
          "a tombstone collision without exchange fingerprints rejects the snapshot");
    CHECK(r.tryReserveOpen("after-ambiguous-snapshot", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "an ambiguous tombstone snapshot stays fail-closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    const std::string sys = "20260710|SHFE|rb|evicted";
    CHECK(r.tryReserveOpen("evicted", "rb", true, 3500, 1) == OpenGate::Ok,
          "evicted-tombstone cleanup setup reserves an order");
    r.onOrderUpdate(1, 7, "evicted", "rb", true, true, '1', '0', 3500, 1,
                    1, sys);
    // Force this tombstone out of the bounded 65,536-entry FIFO while its
    // reported fill still owns a reservation awaiting OnRtnTrade.
    for (int i = 0; i < 65536; ++i) {
      const std::string id = "terminal-" + std::to_string(i);
      r.onUnsupportedOrderUpdate(2, 9, id, '5', "sys-" + id);
    }
    r.onTrade("20260710|SHFE|rb|trade|evicted", "evicted", sys, "rb", true,
              true, '1', 3500, 1);
    CHECK(r.tryReserveOpen("evicted", "rb", true, 3500, 1) == OpenGate::Ok,
          "a released reservation prunes its already-evicted terminal state");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    const std::string sys = "20260710|SHFE|rb|multi";
    r.onUnsupportedOrderUpdate(2, 9, "multi", '3', sys);
    CHECK(r.tryReserveOpen("blocked-by-multi", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a live unsupported order makes position-dependent gates fail closed");
    r.onUnsupportedOrderUpdate(2, 9, "multi", '5', sys);
    r.onUnsupportedOrderUpdate(2, 9, "multi", '3', sys);
    CHECK(r.tryReserveOpen("after-multi-terminal", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "terminal unsupported order ignores a stale replay with the same exchange identity");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    r.onUnsupportedOrderUpdate(2, 9, "multi-reuse", '3',
                               "20260710|SHFE|rb|old-multi");
    r.onUnsupportedOrderUpdate(2, 9, "multi-reuse", '5',
                               "20260710|SHFE|rb|old-multi");
    r.onUnsupportedOrderUpdate(2, 9, "multi-reuse", '3',
                               "20260711|SHFE|rb|new-multi");
    CHECK(r.tryReserveOpen("blocked-by-multi-reuse", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "unsupported terminal-key reuse fails closed instead of being ignored");
    CHECK(r.rebuildOpenReservationsIfRevision(r.revision(), {}),
          "authoritative empty order sync clears unsupported key-reuse ambiguity");
    CHECK(r.tryReserveOpen("after-multi-reuse-sync", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "opens resume after authoritative unsupported-order reconciliation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    r.onUnsupportedOrderUpdate(2, 9, "unsupported-terminal-reuse", '5',
                               "20260710|SHFE|rb|old-unsupported");
    const uint64_t before = r.revision();
    r.onUnsupportedOrderUpdate(2, 9, "unsupported-terminal-reuse", '5',
                               "20260711|SHFE|rb|new-unsupported");
    CHECK(r.revision() > before &&
              r.tryReserveOpen("blocked-unsupported-terminal-reuse", "rb",
                               true, 3500, 1) ==
                  OpenGate::MetadataUnavailable,
          "unsupported terminal reports with different identities fail closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    OpenOrderInfo unsupported;
    unsupported.frontId = 2;
    unsupported.sessionId = 9;
    unsupported.orderRef = "multi-snapshot";
    unsupported.instrumentId = "rb";
    unsupported.vol = 1;
    unsupported.price = 3500;
    unsupported.supportedSingleLeg = false;
    CHECK(r.rebuildOpenReservationsIfRevision(r.revision(), {unsupported}),
          "authoritative order sync records unsupported working orders");
    CHECK(r.tryReserveOpen("blocked-by-multi-snapshot", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "unsupported snapshot rows fail closed instead of being ignored");
    CHECK(r.rebuildOpenReservationsIfRevision(r.revision(), {}),
          "a later empty order snapshot clears unsupported working state");
    CHECK(r.tryReserveOpen("after-multi-sync", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "position-dependent opens resume after the unsupported order is gone");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("unsupported-terminal", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "unsupported-terminal setup reserves a normal opening order");
    r.onUnsupportedOrderUpdate(1, 7, "unsupported-terminal", '5');
    CHECK(r.tryReserveOpen("after-unsupported-terminal", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "an unsupported-format terminal callback releases its known reservation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    r.onUnsupportedOrderUpdate(0, 0, "", '3');
    CHECK(r.tryReserveOpen("malformed-order", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "an unsupported callback without identity fails closed");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}),
          "position sync can still reconcile held exposure");
    CHECK(r.tryReserveOpen("still-blocked", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "position sync does not clear unresolved working-order ambiguity");
    CHECK(r.rebuildOpenReservationsIfRevision(r.revision(), {}),
          "order sync clears identity-less working-order ambiguity");
    CHECK(r.tryReserveOpen("order-reconciled", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "opens resume only after authoritative order reconciliation");
  }
  {
    // Another terminal can deliver Trade before our first Order callback. The
    // later terminal Order must recognize that fill as already applied instead
    // of recreating it as an undelivered-fill reservation.
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    r.onTrade("20260710|SHFE|trade-first", "77", "SHFE|sys77", "rb",
              true, true, '1', 3500, 2);
    r.onOrderUpdate(2, 9, "77", "rb", true, true, '1', '0', 3500, 2,
                    2, "SHFE|sys77");
    r.onTrade("20260710|SHFE|close-after-trade-first", "", "", "rb",
              false, false, '1', 3500, 2);
    // A close deliberately makes the position ledger uncertain (a delayed
    // realtime close for a fill already in a prior snapshot must not double
    // release). Reconcile positions before checking the reservation ledger, as a
    // strategy does to resume opening after a close.
    r.replacePositionsIfRevision(r.revision(), {});
    CHECK(r.tryReserveOpen("mine", "rb", true, 3500, 2) == OpenGate::Ok,
          "trade-before-order does not leave a ghost reservation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("same-ref", "rb", true, 3500, 1) == OpenGate::Ok,
          "cross-terminal trade setup reserves this session's order");
    r.onTrade("20260710|SHFE|other-trade", "same-ref",
              "20260710|SHFE|ag|other-sys", "ag", true, true, '1', 8000, 1);
    CHECK(r.tryReserveOpen("rb-still-reserved", "rb", true, 3500, 1) ==
              OpenGate::VolumeLimit,
          "an unknown concrete OrderSysID cannot release a same-ref order from another session");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    CHECK(r.tryReserveOpen("ambiguous-ref", "rb", true, 3500, 1) == OpenGate::Ok,
          "sys-less trade setup reserves this session's order");
    r.onTrade("20260710|SHFE|no-sys", "ambiguous-ref", "", "rb",
              true, true, '1', 3500, 1);
    CHECK(r.tryReserveOpen("after-sys-less", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a trade without OrderSysID never releases by cross-session OrderRef alone");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    CHECK(r.tryReserveOpen("economic-conflict", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "economic-conflict setup reserves a long order");
    r.onOrderUpdate(1, 7, "economic-conflict", "rb", true, true, '1', '3',
                    3500, 1, 0, "20260710|SHFE|rb|sys-conflict");
    r.onTrade("20260710|SHFE|economic-conflict", "economic-conflict",
              "20260710|SHFE|rb|sys-conflict", "rb", false, true, '1',
              3500, 1);
    CHECK(r.accountMargin() == 35000,
          "a sys-id match with conflicting economics applies the fill exactly once");
    CHECK(r.tryReserveOpen("after-economic-conflict", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "conflicting trade/order economics fail both ledgers closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.onOrderUpdate(2, 9, "sys-a", "rb", true, true, '1', '3', 3500, 1, 0,
                    "20260710|SHFE|rb|duplicate-sys");
    r.onOrderUpdate(3, 10, "sys-b", "rb", true, true, '1', '3', 3500, 1, 0,
                    "20260710|SHFE|rb|duplicate-sys");
    CHECK(r.tryReserveOpen("after-duplicate-sys", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "one OrderSysID cannot be rebound to a different order identity");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    recordTrade(r, "20260710|SHFE|T1", "rb", true, true, 3500, 1);
    recordTrade(r, "20260710|SHFE|T1", "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 35000,
          "duplicate trade callback changes the held ledger exactly once");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    recordTrade(r, "20260710|SHFE|collision", "rb", true, true, 3500, 1);
    recordTrade(r, "20260710|SHFE|collision", "rb", true, true, 3500, 2);
    CHECK(r.accountMargin() == 35000 &&
              r.tryReserveOpen("after-trade-key-collision", "rb", true, 3500, 1) ==
                  OpenGate::MetadataUnavailable,
          "same trade key with different economics fails closed without double-applying");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    for (int i = 0; i < 65536; ++i)
      recordTrade(r, "20260710|SHFE|rb|bounded-" + std::to_string(i), "rb",
                  true, true, 3500, 1);
    recordTrade(r, "20260710|SHFE|rb|bounded-overflow", "rb", true, true,
                3500, 1);
    CHECK(r.tryReserveOpen("after-dedupe-cap", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "an exhausted same-day trade dedupe window fails closed");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}),
          "a same-day authoritative position snapshot still commits safely");
    CHECK(r.tryReserveOpen("same-day-still-blocked", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "same-day sync cannot pretend discarded trade identities are recoverable");
    recordTrade(r, "20260711|SHFE|rb|new-day", "rb", true, true, 3500, 1);
    CHECK(r.tryReserveOpen("new-day-before-sync", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "new-day rollover stays closed until missed exposure is reconciled");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}),
          "new-day authoritative position sync reconciles the exhausted epoch");
    CHECK(r.tryReserveOpen("new-day-reconciled", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "bounded trade dedupe resumes only after day rollover plus reconciliation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.onTrade("20260710|SHFE|bad", "1", "20260710|SHFE|rb|1", "rb",
              true, true, '1', DMAX, 1);
    CHECK(r.tryReserveOpen("after-malformed-trade", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a malformed trade callback makes the position ledger fail closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.onOrderUpdate(2, 9, "bad", "rb", true, true, '1', '3', DMAX, 1, 0);
    CHECK(r.tryReserveOpen("after-malformed-order", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a malformed opening-order callback makes the order ledger fail closed");
  }
  {
    RiskEngine r;
    r.setSession(2, 9);
    CHECK(r.tryReserveOpen("same-ref", "rb", true, 3500, 1) == OpenGate::Ok,
          "identity-conflict setup reserves the original order");
    r.onOrderUpdate(2, 9, "same-ref", "ag", true, true, '1', '3', 8000, 10, 0);
    CHECK(r.tryReserveOpen("after-identity-conflict", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a conflicting order identity makes the order ledger fail closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.onOrderUpdate(2, 9, "modified", "rb", true, true, '1', '3', 3500, 1, 0);
    r.onOrderUpdate(2, 9, "modified", "rb", true, true, '1', '3', 3500, 10, 0);
    CHECK(r.tryReserveOpen("after-volume-increase", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "an increased working remainder requires authoritative order reconciliation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setPositionLimit(r, "rb", true, 10);
    r.onTrade("", "1", "SHFE|unknown", "rb", true, true, '1', 3500, 1);
    CHECK(r.tryReserveOpen("after-keyless", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a trade without a stable id makes position-dependent gates fail closed");
    CHECK(r.replacePositionsIfRevision(r.revision(),
                                       {PositionSeed{"rb", true, 1, 0}}),
          "an authoritative snapshot reconciles a keyless-trade ambiguity");
    CHECK(r.tryReserveOpen("after-keyless-sync", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "position-dependent opens resume after keyless-trade reconciliation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    recordTrade(r, "20261301|SHFE|bad-day", "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 0.0 &&
              r.tryReserveOpen("after-bad-day", "rb", true, 3500, 1) ==
                  OpenGate::MetadataUnavailable,
          "an impossible trading date cannot enter or rotate the trade ledger");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.markTradeLedgerUncertain();
    r.markOrderLedgerUncertain();
    CHECK(r.tryReserveOpen("reconnect-gap", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "reconciliation uncertainty blocks opens even without configured position caps");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}) &&
              r.rebuildOpenReservationsIfRevision(r.revision(), {}),
          "both authoritative snapshots clear reconnect uncertainty");
    CHECK(r.tryReserveOpen("reconnect-ready", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "opens resume after both ledgers reconcile");
  }
  {
    RiskEngine r;
    r.invalidateSession();
    CHECK(r.tryReserveOpen("before-login", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a front transition blocks opens until login publishes a concrete session identity");
    CHECK(!r.allowUnsupportedRiskRequest(),
          "an unknown login identity also blocks unsupported risk mutations");
    r.setSession(1, 7, "9999", "investor");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}) &&
              r.rebuildOpenReservationsIfRevision(r.revision(), {}),
          "a concrete login identity can be reconciled with both authoritative ledgers");
    CHECK(r.tryReserveOpen("after-login-sync", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "opens resume only after login identity and both snapshots are known");
  }
  {
    RiskEngine r;
    constexpr int signedSessionId = -2147483647;
    r.beginSessionTransition(2999, true, "9999", "signed-user");
    CHECK(!r.acceptLoginSession(2999, 1, 0, "9999", "signed-user"),
          "SessionID zero is not a concrete login identity");
    CHECK(r.acceptLoginSession(2999, 1, signedSessionId, "9999", "signed-user"),
          "a negative non-zero signed SessionID is a valid CTP login identity");
    CHECK(r.confirmSessionAccount(1, signedSessionId, "9999", "signed-user"),
          "account confirmation preserves the exact negative SessionID");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}) &&
              r.rebuildOpenReservationsIfRevision(r.revision(), {}) &&
              r.tryReserveOpen("signed-session-ready", "rb", true, 3500, 1) ==
                  OpenGate::Ok,
          "a reconciled negative SessionID session receives normal order authority");
  }
  {
    RiskEngine r;
    r.beginSessionTransition(3001, true);
    r.beginSessionTransition(3002, false); // supersedes the pending login
    CHECK(!r.acceptLoginSession(3001, 1, 7, "old", "old-user"),
          "a delayed login response cannot supersede a newer logout transition");
    r.finishSessionTransition(3002);
    CHECK(r.replacePositionsIfRevision(r.revision(), {}) &&
              r.rebuildOpenReservationsIfRevision(r.revision(), {}) &&
              r.tryReserveOpen("still-logged-out", "rb", true, 3500, 1) ==
                  OpenGate::MetadataUnavailable,
          "authoritative ledgers cannot compensate for a superseded login identity");
    r.beginSessionTransition(3003, true);
    r.finishSessionTransition(3001); // stale final callback must not clear 3003
    CHECK(r.acceptLoginSession(3003, 2, 8, "9999", "new-user"),
          "only the newest correlated login publishes native session identity");
    // The high-level session() confirms the trading account after the SPI login;
    // login alone leaves the account unconfirmed (sessionIdentityKnown_ = false).
    r.confirmSessionAccount(2, 8, "9999", "new-user");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}) &&
              r.rebuildOpenReservationsIfRevision(r.revision(), {}) &&
              r.tryReserveOpen("new-login-ready", "rb", true, 3500, 1) ==
                  OpenGate::Ok,
          "the newest login can trade only after both authoritative ledgers reconcile");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    for (const char *day : {"20260707", "20260708", "20260709", "20260710"})
      recordTrade(r, std::string(day) + "|SHFE|T" + day, "rb", true, true, 1, 1);
    const double before = r.accountMargin();
    recordTrade(r, "20260707|SHFE|T20260707", "rb", true, true, 1, 1);
    CHECK(r.accountMargin() == before,
          "a duplicate from a compacted trading day cannot re-enter the ledger");
    setPositionLimit(r, "rb", true, 100);
    CHECK(r.tryReserveOpen("ambiguous", "rb", true, 1, 1) ==
              OpenGate::MetadataUnavailable,
          "a compacted-day callback makes position-dependent gates fail closed");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}),
          "an authoritative position replacement clears compacted-day ambiguity");
    CHECK(r.tryReserveOpen("after-sync", "rb", true, 1, 1) == OpenGate::Ok,
          "position-dependent opens resume after authoritative reconciliation");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 2);
    CHECK(r.tryReserveOpen("1", "rb", true, 3500, 2) == OpenGate::Ok,
          "position sync regression: working order reserves two lots");
    r.resetPositions();
    CHECK(r.tryReserveOpen("while-reset", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "manual position reset blocks opens pending authoritative replacement");
    CHECK(r.replacePositionsIfRevision(r.revision(), {}),
          "position sync regression applies authoritative flat held snapshot");
    CHECK(r.tryReserveOpen("2", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "position sync does not clear working-order reservations");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    CHECK(r.tryReserveOpen("before", "rb", true, 3500, 1) == OpenGate::Ok,
          "late cap: uncapped working order is still tracked");
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("after", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "late cap: enabling a lot cap counts an existing working order");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    const std::vector<InstrumentPositionLimitUpdate> invalidBatch{
        {"rb", true, 1, true, 1},
        {"", true, 1, false, 0}};
    CHECK(!r.setInstrumentPositionLimits(invalidBatch),
          "an invalid position-limit batch is rejected atomically");
    CHECK(r.tryReserveOpen("no-partial-position-cap", "rb", true, 1, 2) ==
              OpenGate::Ok,
          "a rejected position-limit batch commits no partial cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "ag", 15);
    setSimpleMargin(r, "ag", 1.0);
    const std::vector<InstrumentMarginLimitUpdate> invalidBatch{
        {"ag", 10},
        {"", 10}};
    CHECK(!r.setInstrumentMarginLimits(invalidBatch),
          "an invalid margin-limit batch is rejected atomically");
    CHECK(r.tryReserveOpen("no-partial-margin-cap", "ag", true, 1, 1) ==
              OpenGate::Ok,
          "a rejected margin-limit batch commits no partial cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    CHECK(r.tryReserveOpen("before", "rb", true, 3500, 1) == OpenGate::Ok,
          "late money cap: unknown-cost working order is tracked");
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    CHECK(r.tryReserveOpen("blocked", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "late money cap fails closed while existing working cost is unknown");
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 1.0);
    CHECK(r.tryReserveOpen("after", "rb", true, 3500, 1) == OpenGate::Ok,
          "metadata refresh revalues existing reservations without a resync");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    recordTrade(r, "20260710|local|held", "rb", true, true, 3500, 1);
    RiskLimits c;
    c.maxAccountMargin = 1000000000;
    r.configure(c);
    setFuture(r, "rb", 10);
    setSimpleMargin(r, "rb", 0.1);
    CHECK(r.tryReserveOpen("blocked", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "late money cap does not treat best-effort historical held cost as exact");
    seedHeld(r, "rb", true, 1, 3500);
    CHECK(r.tryReserveOpen("after", "rb", true, 3500, 1) == OpenGate::Ok,
          "authoritative position seed clears unknown held-cost state");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 3);
    std::vector<PositionSeed> seed{{"rb", true, 1, 100}};
    const uint64_t rev = r.revision();
    CHECK(r.replacePositionsIfRevision(rev, seed),
          "atomic position sync accepts an unchanged ledger revision");
    const uint64_t stale = r.revision();
    CHECK(r.tryReserveOpen("1", "rb", true, 3500, 2) == OpenGate::Ok,
          "atomic position sync regression: callback mutation advances revision");
    CHECK(!r.replacePositionsIfRevision(stale, {}),
          "atomic position sync rejects a snapshot raced by a realtime callback");
    CHECK(r.tryReserveOpen("2", "rb", true, 3500, 1) == OpenGate::VolumeLimit,
          "failed atomic sync preserves both held and pending exposure");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.markTradeLedgerUncertain();
    const uint64_t snapshotRevision = r.revision();
    r.markTradeLedgerUncertain();
    CHECK(r.revision() > snapshotRevision,
          "a second ambiguous trade callback advances revision while already uncertain");
    CHECK(!r.replacePositionsIfRevision(snapshotRevision, {}),
          "an already-uncertain trade callback still invalidates an in-flight position snapshot");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    r.markOrderLedgerUncertain();
    const uint64_t snapshotRevision = r.revision();
    r.onUnsupportedOrderUpdate(0, 0, "", '3');
    CHECK(r.revision() > snapshotRevision,
          "a second ambiguous order callback advances revision while already uncertain");
    CHECK(!r.rebuildOpenReservationsIfRevision(snapshotRevision, {}),
          "an already-uncertain order callback still invalidates an in-flight order snapshot");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    setPositionLimit(r, "rb", true, 1);
    CHECK(r.tryReserveOpen("snapshot-kept", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "conflicting snapshot setup reserves existing exposure");
    OpenOrderInfo a;
    a.frontId = 2; a.sessionId = 9; a.orderRef = "dup";
    a.instrumentId = "rb"; a.isLong = true; a.hedgeFlag = '1';
    a.vol = 1; a.traded = 0; a.price = 3500;
    OpenOrderInfo b = a;
    b.vol = 2;
    CHECK(!r.rebuildOpenReservationsIfRevision(r.revision(), {a, b}),
          "a conflicting duplicate order row invalidates the whole snapshot");
    CHECK(r.tryReserveOpen("snapshot-still-kept", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "a rejected conflicting snapshot leaves state intact and fail-closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    OpenOrderInfo a;
    a.frontId = 2; a.sessionId = 9; a.orderRef = "sys1";
    a.instrumentId = "rb"; a.isLong = true; a.hedgeFlag = '1';
    a.vol = 1; a.traded = 0; a.price = 3500;
    a.orderSysKey = "20260710|SHFE|rb|one-sys";
    OpenOrderInfo b = a;
    b.frontId = 3; b.sessionId = 10; b.orderRef = "sys2";
    CHECK(!r.rebuildOpenReservationsIfRevision(r.revision(), {a, b}),
          "an OrderSysID collision invalidates the authoritative order snapshot");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = 100000;
    r.configure(c);
    // Pass a VALID reference (and, for the zero-price case, a valid multiplier)
    // so each order reaches the sub-gate it names instead of failing early at the
    // missing-reference sub-gate — otherwise a fail-open regression in the named
    // gate would be masked (all paths return the same !ok).
    CHECK(!r.check("rb", 3500, 3500, 1).ok,
          "unknown multiplier cannot bypass maxNotional");
    setFuture(r, "rb", DMAX);
    CHECK(!r.check("rb", 3500, 3500, 1).ok,
          "invalid multiplier metadata remains unknown and fails closed");
    setFuture(r, "rb", 10); // valid multiplier: now only the zero limit price fails
    CHECK(!r.check("rb", 0, 3500, 1).ok,
          "zero-price opening order cannot bypass maxNotional");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    setFuture(r, "rb", 10);
    // Valid reference + schedule so ONLY the zero limit price can fail closed
    // (else priceKnown short-circuits on the missing reference and masks it).
    setReference(r, "rb", 3500);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, false);
    CHECK(r.tryReserveOpen("1", "rb", true, 0, 1) ==
              OpenGate::MetadataUnavailable,
          "zero-price opening order cannot bypass maxAccountMargin");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    setFuture(r, "rb", 10);
    // Valid reference + price so the MISSING margin schedule is the gate that
    // fires (not the missing-reference short-circuit).
    setReference(r, "rb", 3500);
    CHECK(r.tryReserveOpen("2", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "missing margin schedule fails closed");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    r.setMarginSchedule("rb", '1', 0.0, 1500.0, 0.0, 1600.0, false);
    recordTrade(r, "20260710|local|T1", "rb", true, true, 3500, 2);
    CHECK(r.accountMargin() == 3000,
          "per-lot margin component is included in held margin");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    setFuture(r, "rb", 10);
    // Valid reference so the ONLY thing missing is the exchange base rate the
    // relative schedule needs — without it, priceKnown would mask this gate.
    setReference(r, "rb", 3500);
    r.setMarginSchedule("rb", '1', 0.1, 0.0, 0.1, 0.0, true);
    CHECK(r.tryReserveOpen("1", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "relative margin schedule fails closed without an exchange base rate");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    setFuture(r, "rb", 10);
    r.setExchangeMarginSchedule("rb", '1', 0.08, 500.0, 0.09, 600.0);
    r.setMarginSchedule("rb", '1', 0.02, 100.0, 0.01, 200.0, true);
    recordTrade(r, "20260710|local|relative", "rb", true, true, 3500, 1);
    CHECK(r.accountMargin() == 4100,
          "relative investor schedule adds exchange by-money and per-lot base");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxAccountMargin = 100000;
    r.configure(c);
    r.setInstrument("m2409-C-2500", 10, '2'); // option
    setSimpleMargin(r, "m2409-C-2500", 0.1);
    CHECK(r.tryReserveOpen("1", "m2409-C-2500", true, 100, 1) ==
              OpenGate::MetadataUnavailable,
          "non-futures products cannot use the futures margin formula");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = 100000;
    r.configure(c);
    r.setInstrument("m2409-C-2500", 10, '2'); // option premium is not futures notional
    CHECK(!r.check("m2409-C-2500", 100, 100, 1, true).ok,
          "non-futures products cannot masquerade premium as futures notional");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = 1;
    c.maxPriceDeviation = 0.01;
    r.configure(c);
    r.reduceOnly();
    CHECK(r.check("rb", 0, 0, 1, false).ok,
          "kill-switch and metadata outage still allow risk-reducing closes");
    bool openAllowed = true;
    { auto guard = r.acquireOrderSend(true, openAllowed); }
    CHECK(!openAllowed, "reduce-only final send barrier rejects an opening order");
    bool closeAllowed = false;
    { auto guard = r.acquireOrderSend(false, closeAllowed); }
    CHECK(closeAllowed, "reduce-only final send barrier admits a closing order");
    r.haltAll();
    CHECK(!r.check("rb", 1, 0, 1, false).ok &&
              !r.allowUnsupportedRiskRequest(),
          "HALT_ALL blocks close inserts and unsupported mutations");
    r.resume();
    CHECK(!r.allowUnsupportedRiskRequest(),
          "NORMAL does not bypass configured controls for unsupported mutations");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    CHECK(r.allowUnsupportedRiskRequest(),
          "unsupported mutation is allowed when every local risk control is disabled");
    r.recordUnsupportedRiskMutation();
    CHECK(r.tryReserveOpen("binding-after-complex", "rb", true, 3500, 1) ==
              OpenGate::Ok,
          "unsupported-mutation history does not disable pure binding mode");
    CHECK(r.bindInsertRequest(104, "binding-after-complex"),
          "pure binding reservation gets an exact request owner");
    r.rollbackInsertRequest(104, "binding-after-complex");
    RiskLimits c;
    c.maxOrderVolume = 10;
    r.configure(c);
    CHECK(!r.allowUnsupportedRiskRequest(),
          "complex insert fails closed when it cannot traverse configured risk");
    CHECK(r.tryReserveOpen("managed-after-complex", "rb", true, 3500, 1) ==
              OpenGate::MetadataUnavailable,
          "enabling local risk after an advanced mutation keeps normal opens fail-closed");
    r.configure({});
    r.reduceOnly();
    CHECK(!r.allowUnsupportedRiskRequest(),
          "parked/complex insert cannot bypass halt");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxPriceDeviation = 0.02;
    c.maxReferenceAgeMs = 100;
    r.configure(c);
    auto snap = std::make_shared<MockSnap>();
    snap->px["rb"] = 3500;
    snap->age = 101;
    r.setMdSnapshot(snap);
    CHECK(r.refPrice("rb") == 0.0 && !r.check("rb", 3500, r.refPrice("rb"), 1).ok,
          "stale market data cannot satisfy a deviation rule");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    std::vector<PositionSeed> good{{"rb", true, 1, 100}};
    CHECK(r.replacePositionsIfRevision(r.revision(), good),
          "snapshot validation setup installs a valid held position");
    const uint64_t rev = r.revision();
    std::vector<PositionSeed> bad{{
        "rb", true, std::numeric_limits<double>::quiet_NaN(), 0}};
    CHECK(!r.replacePositionsIfRevision(rev, bad) &&
              r.accountMargin() == 100,
          "a malformed authoritative snapshot is rejected without clearing live state");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxNotional = std::numeric_limits<double>::infinity();
    r.configure(c);
    setFuture(r, "rb", 10);
    CHECK(!r.check("rb", 1, 0, 1).ok,
          "non-finite native maxNotional fails closed instead of disabling the cap");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrderVolume = -1;
    r.configure(c);
    CHECK(!r.check("rb", 3500, 3500, 1, true).ok &&
              r.check("rb", 3500, 3500, 1, false).ok,
          "a negative native risk field fails closed for opens but preserves closes");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrdersPerSec = std::numeric_limits<double>::infinity();
    r.configure(c);
    CHECK(!r.allowRate(),
          "non-finite native order rate fails closed instead of disabling throttling");
  }
  {
    RiskEngine r;
    r.setSession(1, 7); // standalone: seed a session so onTrade fills accumulate
    RiskLimits c;
    c.maxOrdersPerSec = -1;
    r.configure(c);
    CHECK(!r.allowRate(),
          "negative native order rate fails closed instead of disabling throttling");
  }
  {
    RiskEngine r;
    r.setSession(1, 1);
    CHECK(!setPositionLimit(r, "rb", true,
                            std::numeric_limits<double>::infinity()),
          "non-finite native lot cap is rejected");
  }

  REPORT("RISK-CPP TEST");
}
