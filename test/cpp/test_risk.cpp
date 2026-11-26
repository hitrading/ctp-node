/*
 * test_risk.cpp - standalone unit tests for the stateless RiskEngine and the
 * RateLimiter (src/native/risk.cc).
 *
 * Links risk.cc DIRECTLY - no N-API, no addon, no test-only hooks in the shipped
 * library. Every rule judges one order from the order itself, the latest market
 * reference and static instrument metadata; these tests drive each branch of
 * that judgement, the fail-closed configuration handling, the session/account
 * lifecycle that guards account-bound sends, and the final send barrier.
 * Build + run via `npm run test:cpp`.
 */
#include "risk.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "check.h"

using namespace ctp;

static const double DMAX = std::numeric_limits<double>::max(); // CTP "unset" sentinel
static const double NaN = std::numeric_limits<double>::quiet_NaN();
static const double INF = std::numeric_limits<double>::infinity();

// Mock reference-price source (stands in for the MarketData snapshot cache, so
// the reference tests need no CTP md struct).
struct MockSnap : RefPriceSource {
  std::unordered_map<std::string, double> px;
  uint64_t age = 0;
  bool quote(const std::string &id, double &price,
             uint64_t &ageOut) const override {
    auto it = px.find(id);
    if (it == px.end())
      return false;
    price = it->second;
    ageOut = age;
    return true;
  }
};

static bool sameReason(const char *actual, const char *expected) {
  return actual != nullptr && std::strcmp(actual, expected) == 0;
}

// A rejection must carry the exact reason both in the verdict and in the
// per-thread diagnostic slot that the addon surfaces to JS.
static bool blocked(const RiskEngine &r, const RiskVerdict &v,
                    const char *reason) {
  return !v.ok && sameReason(v.reason, reason) &&
         sameReason(r.lastBlockReason(), reason);
}

// An admitted order carries no reason and clears the diagnostic slot.
static bool passed(const RiskEngine &r, const RiskVerdict &v) {
  return v.ok && v.reason == nullptr && r.lastBlockReason() == nullptr;
}

struct Gate {
  bool allowed;
  bool held; // the send barrier is held for the caller either way
};

static Gate sendGate(RiskEngine &r, bool isOpen) {
  bool allowed = !isOpen; // deliberately wrong: the call must overwrite it
  auto guard = r.acquireOrderSend(isOpen, allowed);
  return {allowed, guard.owns_lock()};
}

static Gate accountGate(RiskEngine &r, bool isOpen, const std::string &broker,
                        const std::string &investor) {
  bool allowed = !isOpen;
  auto guard = r.acquireOrderSendForAccount(isOpen, broker, investor, allowed);
  return {allowed, guard.owns_lock()};
}

// The production handshake: request-correlated login, then the high-level
// session() confirms the trading account.
static bool confirmAccount(RiskEngine &r, int requestId = 1,
                           const std::string &broker = "9999",
                           const std::string &investor = "inv") {
  r.beginSessionTransition(requestId, true, broker, "op");
  return r.acceptLoginSession(requestId, 1, 7, broker, "op") &&
         r.confirmSessionAccount(1, 7, broker, investor);
}

static int countAcquired(RateLimiter &rl, int attempts) {
  int allowed = 0;
  for (int i = 0; i < attempts; ++i)
    if (rl.tryAcquire())
      ++allowed;
  return allowed;
}

static const char *const kAccountMismatch =
    "order account does not match confirmed session";
static const char *const kHalted = "all order inserts halted";
static const char *const kReduceOnly =
    "opening orders blocked in reduce-only mode";
static const char *const kUnsupportedMode =
    "unsupported requests blocked outside NORMAL mode";
static const char *const kSessionUnconfirmed = "session account not confirmed";
static const char *const kUnsupportedControls =
    "unsupported requests blocked while local risk controls are configured";

// An unsupported request refused for exactly this reason.
static bool unsupportedRefused(const RiskEngine &r, const char *reason) {
  return !r.allowUnsupportedRiskRequest() &&
         sameReason(r.lastBlockReason(), reason);
}

// An admitted unsupported request clears the diagnostic slot.
static bool unsupportedAdmitted(const RiskEngine &r) {
  return r.allowUnsupportedRiskRequest() && r.lastBlockReason() == nullptr;
}

static void testRateLimiter() {
  {
    RateLimiter rl;
    CHECK(!rl.enabled(), "rate limiter: a default limiter is disabled");
    CHECK(countAcquired(rl, 100) == 100,
          "rate limiter: a disabled limiter never throttles");
    rl.configure(0, 5);
    CHECK(!rl.enabled() && countAcquired(rl, 10) == 10,
          "rate limiter: rate 0 disables the bucket even with a burst");
  }
  {
    RateLimiter rl;
    rl.configure(2, 3); // slow refill: no token accrues within the test instant
    CHECK(rl.enabled() && countAcquired(rl, 8) == 3,
          "rate limiter: exactly the configured burst passes, the rest is throttled");
  }
  {
    RateLimiter rl;
    rl.configure(5, 0);
    CHECK(countAcquired(rl, 10) == 5,
          "rate limiter: a zero burst defaults to one second of tokens");
  }
  {
    RateLimiter rl;
    rl.configure(0.5, 0);
    CHECK(countAcquired(rl, 3) == 1,
          "rate limiter: a fractional rate gets a one-token minimum burst (never starves forever)");
    RateLimiter sub;
    sub.configure(1, 0.25);
    CHECK(countAcquired(sub, 3) == 1,
          "rate limiter: a sub-token burst is raised to one token");
  }
  {
    RateLimiter rl;
    rl.configure(50, 1); // 50 tokens/s in a one-token bucket
    const bool first = rl.tryAcquire();
    const bool second = rl.tryAcquire();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // ~5 tokens accrue
    const bool refilled = rl.tryAcquire();
    const bool capped = !rl.tryAcquire();
    CHECK(first && !second,
          "rate limiter: an empty bucket throttles the next immediate send");
    CHECK(refilled, "rate limiter: elapsed time refills tokens");
    CHECK(capped, "rate limiter: refill is capped at the burst size");
  }
  {
    RateLimiter rl;
    rl.configure(1, 2);
    const bool drained = countAcquired(rl, 2) == 2 && !rl.tryAcquire();
    rl.configure(1, 2);
    CHECK(drained && rl.tryAcquire(),
          "rate limiter: reconfiguration starts from a full bucket");
  }
  struct BadRate {
    double rate, burst;
    const char *label;
  };
  const BadRate badRates[] = {
      {NaN, 1, "rate limiter: a NaN rate fails closed (enabled, admits nothing)"},
      {INF, 1, "rate limiter: an infinite rate fails closed"},
      {-1, 1, "rate limiter: a negative rate fails closed"},
      {1, NaN, "rate limiter: a NaN burst fails closed"},
      {1, INF, "rate limiter: an infinite burst fails closed"},
      {1, -1, "rate limiter: a negative burst fails closed"},
  };
  for (const auto &b : badRates) {
    RateLimiter rl;
    rl.configure(b.rate, b.burst);
    CHECK(rl.enabled() && countAcquired(rl, 3) == 0, b.label);
  }
  {
    RateLimiter rl;
    rl.configure(NaN, 1);
    rl.configure(1, 1);
    CHECK(rl.tryAcquire(),
          "rate limiter: a valid reconfiguration replaces a fail-closed one");
  }
}

static void testConfigure() {
  {
    RiskEngine r; // all-zero config = every control disabled
    CHECK(passed(r, r.check("rb", 1e9, 0, 1000000)),
          "disabled config: a default engine blocks nothing");
    r.configure(RiskLimits{});
    CHECK(passed(r, r.check("rb", 1e9, 0, 1000000)) && r.allowRate() &&
              r.allowCloseRate(),
          "disabled config: an explicit all-zero config keeps every rule off");
  }
  // Every malformed field fails the whole configuration closed: native callers
  // that bypass the TS guards must never silently disable a control.
  struct BadLimit {
    void (*apply)(RiskLimits &);
    int rateGate; // 0 none, 1 opening bucket, 2 closing bucket
    const char *label;
  };
  const BadLimit badLimits[] = {
      {[](RiskLimits &c) { c.maxOrderVolume = -1; }, 0, "negative maxOrderVolume"},
      {[](RiskLimits &c) { c.maxPriceDeviation = NaN; }, 0, "NaN maxPriceDeviation"},
      {[](RiskLimits &c) { c.maxPriceDeviation = -0.01; }, 0, "negative maxPriceDeviation"},
      {[](RiskLimits &c) { c.maxNotional = INF; }, 0, "infinite maxNotional"},
      {[](RiskLimits &c) { c.maxNotional = -1; }, 0, "negative maxNotional"},
      {[](RiskLimits &c) { c.maxOrdersPerSec = NaN; }, 1, "NaN maxOrdersPerSec"},
      {[](RiskLimits &c) { c.maxOrdersPerSec = -1; }, 1, "negative maxOrdersPerSec"},
      {[](RiskLimits &c) { c.orderBurst = INF; }, 1, "infinite orderBurst"},
      {[](RiskLimits &c) { c.orderBurst = -1; }, 1, "negative orderBurst"},
      {[](RiskLimits &c) { c.maxCloseOrdersPerSec = NaN; }, 2, "NaN maxCloseOrdersPerSec"},
      {[](RiskLimits &c) { c.maxCloseOrdersPerSec = -1; }, 2, "negative maxCloseOrdersPerSec"},
      {[](RiskLimits &c) { c.closeOrderBurst = INF; }, 2, "infinite closeOrderBurst"},
      {[](RiskLimits &c) { c.closeOrderBurst = -1; }, 2, "negative closeOrderBurst"},
      {[](RiskLimits &c) { c.maxReferenceAgeMs = NaN; }, 0, "NaN maxReferenceAgeMs"},
      {[](RiskLimits &c) { c.maxReferenceAgeMs = 0; }, 0, "zero maxReferenceAgeMs"},
      {[](RiskLimits &c) { c.maxReferenceAgeMs = -5; }, 0, "negative maxReferenceAgeMs"},
  };
  for (const auto &b : badLimits) {
    RiskEngine r;
    RiskLimits c;
    b.apply(c);
    r.configure(c);
    const std::string label(b.label);
    CHECK(blocked(r, r.check("rb", 3500, 3500, 1, true),
                  "risk configuration is invalid"),
          (label + ": opening orders fail closed").c_str());
    CHECK(passed(r, r.check("rb", 3500, 3500, 1, false)),
          (label + ": risk-reducing closes stay available").c_str());
    CHECK(!r.allowUnsupportedRiskRequest(),
          (label + ": unsupported requests fail closed").c_str());
    if (b.rateGate == 1)
      CHECK(!r.allowRate() && r.allowCloseRate(),
            (label + ": the opening bucket fails closed").c_str());
    if (b.rateGate == 2)
      CHECK(r.allowRate() && !r.allowCloseRate(),
            (label + ": the closing bucket fails closed").c_str());
    r.configure(RiskLimits{});
    CHECK(passed(r, r.check("rb", 3500, 3500, 1, true)) &&
              r.allowUnsupportedRiskRequest() && r.allowRate() &&
              r.allowCloseRate(),
          (label + ": a complete valid configure() clears the fail-closed state").c_str());
  }
  {
    RiskEngine r;
    RiskLimits c;
    c.maxReferenceAgeMs = 0;
    r.configure(c);
    r.reduceOnly();
    CHECK(blocked(r, r.check("rb", 3500, 3500, 1, true),
                  "risk configuration is invalid"),
          "an invalid configuration is reported before the reduce-only mode");
  }
}

static void testCheck() {
  {
    RiskEngine r;
    CHECK(blocked(r, r.check("rb", 3500, 0, 0), "order volume must be positive"),
          "check: zero volume is rejected");
    CHECK(blocked(r, r.check("rb", 3500, 0, -3, false),
                  "order volume must be positive"),
          "check: a negative volume is rejected even for a close");
    CHECK(blocked(r, r.check("rb", NaN, 0, 1),
                  "order price must be finite and non-negative"),
          "check: a NaN price is rejected");
    CHECK(blocked(r, r.check("rb", INF, 0, 1, false),
                  "order price must be finite and non-negative"),
          "check: an infinite price is rejected even for a close");
    CHECK(blocked(r, r.check("rb", -1, 0, 1),
                  "order price must be finite and non-negative"),
          "check: a negative price is rejected");
    CHECK(passed(r, r.check("rb", 0, 0, 1)),
          "check: a zero (market) price passes structural validation with no reference rule");
    r.haltAll();
    CHECK(blocked(r, r.check("rb", 3500, 0, 0), "order volume must be positive"),
          "check: structural validation is reported before the halt");
  }
  {
    RiskEngine r;
    r.haltAll();
    CHECK(r.mode() == RiskMode::HaltAll, "mode() reports HALT_ALL");
    CHECK(blocked(r, r.check("rb", 3500, 3500, 1, true), kHalted),
          "HALT_ALL blocks opening orders");
    CHECK(blocked(r, r.check("rb", 3500, 3500, 1, false), kHalted),
          "HALT_ALL blocks closing orders too");
    r.reduceOnly();
    CHECK(r.mode() == RiskMode::ReduceOnly, "mode() reports REDUCE_ONLY");
    CHECK(blocked(r, r.check("rb", 3500, 3500, 1, true), kReduceOnly),
          "REDUCE_ONLY blocks risk-increasing orders");
    CHECK(passed(r, r.check("rb", 3500, 3500, 1, false)),
          "REDUCE_ONLY keeps risk-reducing closes available");
    r.resume();
    CHECK(r.mode() == RiskMode::Normal &&
              passed(r, r.check("rb", 3500, 3500, 1, true)),
          "resume() returns to NORMAL and admits opening orders again");
  }
  {
    RiskEngine r;
    RiskLimits c;
    c.maxOrderVolume = 1;
    c.maxPriceDeviation = 0.01;
    c.maxNotional = 1;
    r.configure(c);
    CHECK(passed(r, r.check("rb", 0, 0, 100, false)),
          "closing orders skip the volume, deviation and notional rules (metadata outage tolerated)");
  }
  {
    RiskEngine r;
    RiskLimits c;
    c.maxOrderVolume = 10;
    r.configure(c);
    CHECK(passed(r, r.check("rb", 3500, 0, 10)),
          "maxOrderVolume: an order at the cap is allowed");
    CHECK(blocked(r, r.check("rb", 3500, 0, 11),
                  "order volume exceeds maxOrderVolume"),
          "maxOrderVolume: an order over the cap is blocked");
  }
  {
    RiskEngine r;
    RiskLimits c;
    c.maxPriceDeviation = 0.02;
    r.configure(c);
    const char *noRef = "reference price unavailable or stale";
    CHECK(blocked(r, r.check("rb", 1000, 0, 1), noRef),
          "maxPriceDeviation: a missing reference fails closed");
    CHECK(blocked(r, r.check("rb", 1000, DMAX, 1), noRef),
          "maxPriceDeviation: a DBL_MAX sentinel reference fails closed");
    CHECK(blocked(r, r.check("rb", 1000, NaN, 1), noRef),
          "maxPriceDeviation: a NaN reference fails closed");
    CHECK(blocked(r, r.check("rb", 1000, -5, 1), noRef),
          "maxPriceDeviation: a negative reference fails closed");
    const char *noPrice = "order price unavailable for deviation check";
    CHECK(blocked(r, r.check("rb", 0, 1000, 1), noPrice),
          "maxPriceDeviation: a zero (market) price cannot be judged and fails closed");
    CHECK(blocked(r, r.check("rb", DMAX, 1000, 1), noPrice),
          "maxPriceDeviation: a DBL_MAX limit price fails closed");
    CHECK(passed(r, r.check("rb", 1020, 1000, 1)) &&
              passed(r, r.check("rb", 980, 1000, 1)),
          "maxPriceDeviation: exactly 2% above or below the reference is allowed");
    const char *tooFar = "order price deviates too far from reference";
    CHECK(blocked(r, r.check("rb", 1021, 1000, 1), tooFar) &&
              blocked(r, r.check("rb", 979, 1000, 1), tooFar),
          "maxPriceDeviation: beyond 2% in either direction is blocked");
  }
  {
    RiskEngine r;
    RiskLimits c;
    c.maxNotional = 1000000;
    r.configure(c);
    CHECK(blocked(r, r.check("rb", 3500, 0, 1),
                  "reference price unavailable or stale for maxNotional"),
          "maxNotional: a missing reference fails closed");
    CHECK(blocked(r, r.check("rb", 0, 3500, 1),
                  "order price unavailable for maxNotional"),
          "maxNotional: a zero (market) price fails closed");
    CHECK(blocked(r, r.check("rb", 3500, 3500, 1),
                  "instrument multiplier unavailable"),
          "maxNotional: an unknown multiplier fails closed");
    r.setInstrument("rb", 10, '1');
    CHECK(passed(r, r.check("rb", 3500, 3500, 28)),
          "maxNotional: 3500*28*10 = 0.98M under 1M is allowed (multiplier-aware)");
    CHECK(passed(r, r.check("rb", 1000, 1000, 100)),
          "maxNotional: exactly the cap is allowed");
    const char *over = "order notional exceeds maxNotional";
    CHECK(blocked(r, r.check("rb", 3500, 3500, 29), over),
          "maxNotional: 3500*29*10 = 1.015M over 1M is blocked");
    CHECK(blocked(r, r.check("rb", 1, 3500, 29), over),
          "maxNotional: a low limit is valued at the higher market reference");
    CHECK(blocked(r, r.check("rb", 3600, 3400, 28), over),
          "maxNotional: a limit above the reference is valued at the limit");
    r.setInstrument("tas", 10, '7');
    CHECK(passed(r, r.check("tas", 3500, 3500, 1)),
          "maxNotional: TAS products use the futures notional formula");
    const char *unsupported = "instrument product class unsupported for maxNotional";
    r.setInstrument("opt", 10, '2');
    CHECK(blocked(r, r.check("opt", 100, 100, 1), unsupported),
          "maxNotional: an option premium cannot masquerade as futures notional");
    r.setInstrument("combo", 10, '3');
    CHECK(blocked(r, r.check("combo", 100, 100, 1), unsupported),
          "maxNotional: a combination product fails closed");
  }
  {
    RiskEngine r;
    RiskLimits c;
    c.maxOrderVolume = 1;
    r.configure(c);
    CHECK(passed(r, r.check("rb", 1, 0, 1)), "thread-local reason: setup admits");
    const char *otherReason = nullptr;
    std::thread other([&] {
      r.check("rb", 1, 0, 5);
      otherReason = r.lastBlockReason();
    });
    other.join();
    CHECK(sameReason(otherReason, "order volume exceeds maxOrderVolume") &&
              r.lastBlockReason() == nullptr,
          "lastBlockReason is per thread: another thread's rejection does not overwrite it");
  }
}

static void testReferencePrice() {
  RiskEngine r;
  CHECK(r.refPrice("rb") == 0.0, "refPrice: no snapshot attached reads 0");
  auto snap = std::make_shared<MockSnap>();
  snap->px["rb"] = 3500;
  snap->px["dmax"] = DMAX;
  snap->px["nan"] = NaN;
  snap->px["inf"] = INF;
  snap->px["zero"] = 0;
  snap->px["neg"] = -1;
  r.setMdSnapshot(snap);
  CHECK(r.refPrice("rb") == 3500, "refPrice reads the attached MD snapshot");
  CHECK(r.refPrice("zz") == 0.0,
        "refPrice: an instrument absent from the snapshot reads 0");
  CHECK(r.refPrice("dmax") == 0.0 && r.refPrice("nan") == 0.0 &&
            r.refPrice("inf") == 0.0 && r.refPrice("zero") == 0.0 &&
            r.refPrice("neg") == 0.0,
        "refPrice rejects DBL_MAX / NaN / Inf / zero / negative snapshot prices");
  snap->age = 3000;
  CHECK(r.refPrice("rb") == 3500,
        "refPrice: a quote exactly at the default 3000 ms window is fresh");
  snap->age = 3001;
  CHECK(r.refPrice("rb") == 0.0,
        "refPrice: a quote older than the default window is stale");
  RiskLimits c;
  c.maxReferenceAgeMs = 100.9;
  r.configure(c);
  snap->age = 100;
  const bool fresh = r.refPrice("rb") == 3500;
  snap->age = 101;
  CHECK(fresh && r.refPrice("rb") == 0.0,
        "refPrice: a fractional window is truncated to whole milliseconds");
  c.maxReferenceAgeMs = 1e20;
  r.configure(c);
  snap->age = std::numeric_limits<uint64_t>::max();
  CHECK(r.refPrice("rb") == 3500,
        "refPrice: a window beyond the uint64 range never expires a quote");
  r.setMdSnapshot(nullptr);
  CHECK(r.refPrice("rb") == 0.0, "refPrice: a detached snapshot reads 0 again");
  {
    RiskEngine stale;
    RiskLimits dev;
    dev.maxPriceDeviation = 0.02;
    dev.maxReferenceAgeMs = 100;
    stale.configure(dev);
    auto old = std::make_shared<MockSnap>();
    old->px["rb"] = 3500;
    old->age = 101;
    stale.setMdSnapshot(old);
    CHECK(blocked(stale, stale.check("rb", 3500, stale.refPrice("rb"), 1),
                  "reference price unavailable or stale"),
          "stale market data cannot satisfy an enabled deviation rule");
  }
}

static void testInstrumentMetadata() {
  RiskEngine r;
  RiskLimits c;
  c.maxNotional = 1e9;
  r.configure(c);
  const char *unknown = "instrument multiplier unavailable";
  auto known = [&](const std::string &id) {
    return passed(r, r.check(id, 100, 100, 1));
  };
  r.setInstrument("", 10, '1');
  CHECK(blocked(r, r.check("", 100, 100, 1), unknown),
        "setInstrument ignores an empty instrument id");
  r.setInstrument("rb", 10, '1');
  r.setInstrument("ag", 15, '1');
  CHECK(known("rb") && known("ag"), "setInstrument records per-instrument multipliers");
  const double badMultipliers[] = {0, -1, NaN, INF, DMAX};
  bool allCleared = true;
  for (double bad : badMultipliers) {
    r.setInstrument("rb", 10, '1');
    r.setInstrument("rb", bad, '1');
    allCleared = blocked(r, r.check("rb", 100, 100, 1), unknown) && allCleared;
  }
  CHECK(allCleared,
        "an unusable multiplier (0 / negative / NaN / Inf / DBL_MAX) clears the old value instead of retaining it");
  r.setInstrument("rb", 10, '2');
  const bool option = blocked(r, r.check("rb", 100, 100, 1),
                              "instrument product class unsupported for maxNotional");
  r.setInstrument("rb", 10, '1');
  CHECK(option && known("rb"),
        "a metadata refresh replaces the product class of an instrument");
  r.invalidateInstrument("rb");
  CHECK(blocked(r, r.check("rb", 100, 100, 1), unknown) && known("ag"),
        "invalidateInstrument(id) clears only that instrument before its query result arrives");
  r.setInstrument("rb", 10, '1');
  r.invalidateInstrument("");
  CHECK(blocked(r, r.check("rb", 100, 100, 1), unknown) &&
            blocked(r, r.check("ag", 100, 100, 1), unknown),
        "invalidateInstrument(\"\") clears every instrument");
  r.setInstrument("rb", 10, '1');
  CHECK(known("rb"), "metadata can be repopulated after a full invalidation");
}

static void testModesAndSendBarrier() {
  {
    RiskEngine r;
    CHECK(r.mode() == RiskMode::Normal, "a new engine starts in NORMAL");
    const Gate open = sendGate(r, true), close = sendGate(r, false);
    CHECK(open.allowed && open.held && close.allowed && close.held,
          "NORMAL admits opening and closing sends under the held send barrier");
    r.reduceOnly();
    const Gate reducedOpen = sendGate(r, true);
    CHECK(!reducedOpen.allowed && reducedOpen.held &&
              sameReason(r.lastBlockReason(), kReduceOnly),
          "REDUCE_ONLY refuses an opening send at the final barrier");
    CHECK(sendGate(r, false).allowed,
          "REDUCE_ONLY still admits a closing send at the final barrier");
    r.haltAll();
    const Gate haltedOpen = sendGate(r, true);
    const bool openReason = sameReason(r.lastBlockReason(), kHalted);
    const Gate haltedClose = sendGate(r, false);
    CHECK(!haltedOpen.allowed && openReason && !haltedClose.allowed &&
              sameReason(r.lastBlockReason(), kHalted),
          "HALT_ALL refuses opening and closing sends at the final barrier");
    r.resume();
    CHECK(sendGate(r, true).allowed && sendGate(r, false).allowed,
          "resume() reopens the final send barrier");
  }
  {
    RiskEngine r;
    CHECK(r.allowRate() && r.allowCloseRate(),
          "unconfigured order buckets never throttle");
    RiskLimits c;
    c.maxOrdersPerSec = 2;
    c.orderBurst = 3;
    c.maxCloseOrdersPerSec = 1;
    c.closeOrderBurst = 1;
    r.configure(c);
    int opens = 0;
    for (int i = 0; i < 6; ++i)
      if (r.allowRate())
        ++opens;
    const bool closeFirst = r.allowCloseRate();
    const bool closeSecond = r.allowCloseRate();
    CHECK(opens == 3, "allowRate admits exactly the opening-order burst");
    CHECK(closeFirst && !closeSecond,
          "allowCloseRate uses an independent bucket that opening exhaustion cannot starve");
  }
  {
    RiskEngine r;
    bool allowed = false;
    auto guard = r.acquireOrderSend(true, allowed);
    r.haltAll(); // same thread: an inline vendor callback may take the barrier
    CHECK(allowed && r.mode() == RiskMode::HaltAll,
          "the send barrier is re-entrant on the sending thread (no self-deadlock)");
  }
  {
    RiskEngine r;
    std::atomic<bool> started{false}, done{false};
    std::thread changer;
    {
      bool allowed = false;
      auto guard = r.acquireOrderSend(true, allowed);
      changer = std::thread([&] {
        started = true;
        r.haltAll();
        done = true;
      });
      while (!started)
        std::this_thread::yield();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(allowed && !done && r.mode() == RiskMode::Normal,
            "a mode change waits while an admitted send holds the barrier");
    }
    changer.join();
    CHECK(done && r.mode() == RiskMode::HaltAll,
          "the mode change lands once the send barrier is released");
  }
  {
    RiskEngine r;
    const bool confirmed = confirmAccount(r);
    std::atomic<bool> started{false}, done{false};
    std::thread transition;
    {
      bool allowed = false;
      auto guard = r.acquireOrderSendForAccount(true, "9999", "inv", allowed);
      transition = std::thread([&] {
        started = true;
        r.invalidateSession();
        done = true;
      });
      while (!started)
        std::this_thread::yield();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(confirmed && allowed && !done && r.acceptsOrderAccount("9999", "inv"),
            "a session transition cannot revoke the account mid-send: it waits for the barrier");
    }
    transition.join();
    CHECK(done && !r.acceptsOrderAccount("9999", "inv"),
          "the session transition revokes the account once the send leaves the barrier");
  }
}

static void testLoginLifecycle() {
  struct LoginCase {
    int requestId, frontId, sessionId;
    const char *broker, *user, *label;
  };
  const LoginCase rejectedLogins[] = {
      {0, 1, 7, "9999", "op", "acceptLoginSession rejects request id 0"},
      {-41, 1, 7, "9999", "op", "acceptLoginSession rejects a negative request id"},
      {42, 1, 7, "9999", "op", "acceptLoginSession rejects an uncorrelated request id"},
      {41, 1, 7, "", "op", "acceptLoginSession rejects an empty BrokerID"},
      {41, 1, 7, "9999", "", "acceptLoginSession rejects an empty UserID"},
      {41, 0, 7, "9999", "op", "acceptLoginSession rejects FrontID 0"},
      {41, -1, 7, "9999", "op", "acceptLoginSession rejects a negative FrontID"},
      {41, 1, 0, "9999", "op", "acceptLoginSession rejects SessionID 0"},
      {41, 1, 7, "8888", "op", "acceptLoginSession rejects a BrokerID other than the requested one"},
      {41, 1, 7, "9999", "other", "acceptLoginSession rejects a UserID other than the requested one"},
  };
  for (const auto &c : rejectedLogins) {
    RiskEngine r;
    r.beginSessionTransition(41, true, "9999", "op");
    const bool rejected = !r.acceptLoginSession(c.requestId, c.frontId,
                                                c.sessionId, c.broker, c.user);
    CHECK(rejected && r.acceptLoginSession(41, 1, 7, "9999", "op"), c.label);
  }
  {
    RiskEngine r;
    r.beginSessionTransition(43, false, "9999", "op");
    CHECK(!r.acceptLoginSession(43, 1, 7, "9999", "op"),
          "a login response cannot complete a logout transition");
  }
  {
    RiskEngine r;
    r.beginSessionTransition(0, true);
    const bool zero = !r.acceptLoginSession(0, 1, 7, "9999", "op");
    r.beginSessionTransition(-9, true);
    CHECK(zero && !r.acceptLoginSession(-9, 1, 7, "9999", "op"),
          "a non-positive transition id never correlates a login response");
  }
  {
    RiskEngine r;
    r.beginSessionTransition(44, true);
    CHECK(r.acceptLoginSession(44, 1, 7, "any-broker", "any-user"),
          "a transition without a requested identity accepts the concrete response identity");
    CHECK(!r.acceptLoginSession(44, 1, 7, "any-broker", "any-user"),
          "a correlated login response is consumed exactly once");
    CHECK(!r.acceptsOrderAccount("any-broker", "inv") &&
              !r.allowUnsupportedRiskRequest(),
          "a login alone grants no order authority until the account is confirmed");
  }
  {
    RiskEngine r;
    constexpr int signedSessionId = -2147483647;
    r.beginSessionTransition(45, true, "9999", "op");
    CHECK(r.acceptLoginSession(45, 1, signedSessionId, "9999", "op") &&
              r.confirmSessionAccount(1, signedSessionId, "9999", "inv") &&
              r.acceptsOrderAccount("9999", "inv"),
          "a negative non-zero signed SessionID is a valid CTP identity end to end");
  }
  {
    RiskEngine r;
    r.beginSessionTransition(60, true, "9999", "op");
    r.finishSessionTransition(0);
    r.finishSessionTransition(-1);
    r.finishSessionTransition(59);
    CHECK(r.acceptLoginSession(60, 1, 7, "9999", "op"),
          "finishSessionTransition ignores non-positive and stale ids");
    r.beginSessionTransition(61, true, "9999", "op");
    r.finishSessionTransition(61);
    CHECK(!r.acceptLoginSession(61, 1, 7, "9999", "op"),
          "a finished transition cannot be completed by a late response");
  }
  {
    RiskEngine r;
    r.beginSessionTransition(70, true, "9999", "op");
    r.beginSessionTransition(71, false, "9999", "op"); // supersedes the login
    CHECK(!r.acceptLoginSession(70, 1, 7, "9999", "op"),
          "a delayed login response cannot override a newer logout transition");
    r.finishSessionTransition(70); // stale final callback of the old login
    CHECK(r.timeoutSessionTransition(71),
          "a stale finish leaves the newer transition pending");
  }
  {
    RiskEngine r;
    const bool confirmed = confirmAccount(r);
    r.beginSessionTransition(80, true, "9999", "op");
    CHECK(confirmed && !r.acceptsOrderAccount("9999", "inv") &&
              !r.confirmSessionAccount(1, 7, "9999", "inv"),
          "beginSessionTransition revokes the confirmed account and the old login identity");
  }
  {
    RiskEngine r;
    CHECK(!r.timeoutSessionTransition(0) && !r.timeoutSessionTransition(-1),
          "timeoutSessionTransition ignores non-positive ids");
    r.beginSessionTransition(90, true, "9999", "op");
    CHECK(!r.timeoutSessionTransition(89),
          "a timeout for another id returns false (its own callback already settled)");
    CHECK(r.timeoutSessionTransition(90),
          "a timeout of the pending id wins and revokes it");
    CHECK(!r.acceptLoginSession(90, 1, 7, "9999", "op"),
          "a revoked transition rejects its late login response");
    CHECK(!r.timeoutSessionTransition(90),
          "a revoked transition times out only once");
    r.beginSessionTransition(91, true, "9999", "op");
    const bool login = r.acceptLoginSession(91, 1, 7, "9999", "op");
    CHECK(login && !r.timeoutSessionTransition(91) &&
              r.confirmSessionAccount(1, 7, "9999", "inv"),
          "a login callback that beat the timer keeps its identity (timeout returns false)");
  }
}

static void testAccountConfirmation() {
  struct ConfirmCase {
    bool login;
    int frontId, sessionId;
    const char *broker, *investor, *label;
  };
  const ConfirmCase rejected[] = {
      {false, 1, 7, "9999", "inv", "confirmSessionAccount requires a correlated login first"},
      {true, 0, 7, "9999", "inv", "confirmSessionAccount rejects FrontID 0"},
      {true, 1, 0, "9999", "inv", "confirmSessionAccount rejects SessionID 0"},
      {true, 2, 7, "9999", "inv", "confirmSessionAccount rejects another FrontID"},
      {true, 1, 8, "9999", "inv", "confirmSessionAccount rejects another SessionID"},
      {true, 1, 7, "", "inv", "confirmSessionAccount rejects an empty BrokerID"},
      {true, 1, 7, "9999", "", "confirmSessionAccount rejects an empty InvestorID"},
      {true, 1, 7, "8888", "inv", "confirmSessionAccount rejects a BrokerID other than the login's"},
  };
  for (const auto &c : rejected) {
    RiskEngine r;
    if (c.login) {
      r.beginSessionTransition(50, true, "9999", "op");
      r.acceptLoginSession(50, 1, 7, "9999", "op");
    }
    const bool refused = !r.confirmSessionAccount(c.frontId, c.sessionId,
                                                  c.broker, c.investor);
    CHECK(refused && !r.acceptsOrderAccount("9999", "inv") &&
              (!c.login || r.confirmSessionAccount(1, 7, "9999", "inv")),
          c.label);
  }
  {
    RiskEngine r;
    CHECK(confirmAccount(r) && r.acceptsOrderAccount("9999", "inv"),
          "a confirmed InvestorID may differ from the login operator UserID");
  }
}

static void testSessionSeedingAndInvalidation() {
  {
    RiskEngine r;
    r.setSession(1, 7);
    CHECK(r.acceptsOrderAccount("9999", "inv") && r.acceptsOrderAccount("", ""),
          "a standalone numeric session without account text accepts any account");
    r.setSession(1, 8);
    CHECK(!r.acceptsOrderAccount("9999", "inv"),
          "once a session was seeded, a re-seed must carry a concrete account");
    r.setSession(1, 8, "9999", "");
    const bool noInvestor = !r.acceptsOrderAccount("9999", "inv");
    r.setSession(1, 8, "", "inv");
    CHECK(noInvestor && !r.acceptsOrderAccount("9999", "inv"),
          "a re-seed with a missing BrokerID or InvestorID stays unconfirmed");
    r.setSession(1, 8, "9999", "inv");
    CHECK(r.acceptsOrderAccount("9999", "inv") && !r.acceptsOrderAccount("9999", "x"),
          "a re-seed with a concrete account confirms exactly that account");
  }
  {
    RiskEngine broker, investor;
    broker.setSession(1, 7, "9999", "");
    investor.setSession(1, 7, "", "inv");
    CHECK(!broker.acceptsOrderAccount("9999", "inv") &&
              !broker.acceptsOrderAccount("9999", "") &&
              !investor.acceptsOrderAccount("9999", "inv") &&
              !investor.acceptsOrderAccount("", "inv"),
          "partial standalone account text never matches an order account");
  }
  {
    RiskEngine r;
    r.beginSessionTransition(100, true, "9999", "op");
    r.setSession(1, 7, "9999", "inv");
    CHECK(!r.acceptLoginSession(100, 1, 7, "9999", "op") &&
              r.acceptsOrderAccount("9999", "inv"),
          "setSession clears a pending transition and installs the seeded identity");
  }
  {
    RiskEngine r;
    r.invalidateSession();
    CHECK(!r.acceptsOrderAccount("9999", "inv") && !r.allowUnsupportedRiskRequest(),
          "after a front transition no account is accepted until a fresh identity");
    r.setSession(1, 7);
    CHECK(!r.acceptsOrderAccount("9999", "inv"),
          "after a front transition a numeric-only seed is not a concrete identity");
  }
  {
    RiskEngine r;
    const bool confirmed = confirmAccount(r);
    r.invalidateSession();
    CHECK(confirmed && !r.acceptsOrderAccount("9999", "inv"),
          "invalidateSession revokes the confirmed account");
    r.beginSessionTransition(110, true, "9999", "op");
    r.invalidateSession();
    const bool pendingDropped = !r.acceptLoginSession(110, 1, 7, "9999", "op");
    r.beginSessionTransition(111, true, "9999", "op");
    r.acceptLoginSession(111, 1, 7, "9999", "op");
    r.invalidateSession();
    CHECK(pendingDropped && !r.confirmSessionAccount(1, 7, "9999", "inv"),
          "invalidateSession drops a pending login and forgets an unconfirmed login identity");
  }
}

static void testAccountScope() {
  {
    RiskEngine r;
    const Gate unknown = accountGate(r, true, "9999", "inv");
    CHECK(!unknown.allowed && unknown.held &&
              sameReason(r.lastBlockReason(), kAccountMismatch) &&
              !r.acceptsOrderAccount("9999", "inv"),
          "an unknown session identity is ambiguous: account-bound sends fail closed");
  }
  RiskEngine r;
  CHECK(confirmAccount(r), "account scope setup confirms 9999/inv");
  const Gate open = accountGate(r, true, "9999", "inv");
  const Gate close = accountGate(r, false, "9999", "inv");
  CHECK(open.allowed && open.held && close.allowed && close.held &&
            r.acceptsOrderAccount("9999", "inv"),
        "the confirmed account is the current scope for opening and closing sends");
  struct ScopeCase {
    std::string broker, investor;
    const char *label;
  };
  const ScopeCase refused[] = {
      {"8888", "inv", "a foreign BrokerID is refused"},
      {"9999", "other", "a foreign InvestorID is refused"},
      {"", "inv", "an empty BrokerID is ambiguous and refused"},
      {"9999", "", "an empty InvestorID is ambiguous and refused"},
      {" 9999", "inv", "a leading-space BrokerID is ambiguous and refused"},
      {"9999", "inv ", "a trailing-space InvestorID is ambiguous and refused"},
      {"\t9999", "inv", "a control-character padded BrokerID is ambiguous and refused"},
      {std::string("99\0" "99", 5), "inv", "a BrokerID with an embedded NUL is ambiguous and refused"},
  };
  for (const auto &c : refused) {
    const Gate g = accountGate(r, false, c.broker, c.investor);
    CHECK(!g.allowed && g.held && sameReason(r.lastBlockReason(), kAccountMismatch) &&
              !r.acceptsOrderAccount(c.broker, c.investor),
          c.label);
  }
  r.reduceOnly();
  const Gate reducedOpen = accountGate(r, true, "9999", "inv");
  const bool reducedReason = sameReason(r.lastBlockReason(), kReduceOnly);
  const Gate reducedClose = accountGate(r, false, "9999", "inv");
  const Gate reducedForeign = accountGate(r, false, "8888", "inv");
  CHECK(!reducedOpen.allowed && reducedReason && reducedClose.allowed &&
            !reducedForeign.allowed &&
            sameReason(r.lastBlockReason(), kAccountMismatch),
        "REDUCE_ONLY is judged before the account; closes still need the confirmed account");
  r.haltAll();
  const Gate haltedForeign = accountGate(r, true, "8888", "inv");
  CHECK(!haltedForeign.allowed && sameReason(r.lastBlockReason(), kHalted) &&
            !accountGate(r, false, "9999", "inv").allowed,
        "HALT_ALL refuses every account-bound send with the halt reason");
}

static void testUnsupportedRequests() {
  RiskEngine r;
  CHECK(unsupportedAdmitted(r),
        "a standalone engine with no front lifecycle and no rule admits unsupported requests");
  r.reduceOnly();
  const bool reduced = unsupportedRefused(r, kUnsupportedMode);
  r.haltAll();
  const bool halted = unsupportedRefused(r, kUnsupportedMode);
  r.resume();
  CHECK(reduced && halted && unsupportedAdmitted(r),
        "unsupported requests pass only in NORMAL mode, naming the mode otherwise");
  r.invalidateSession();
  CHECK(unsupportedRefused(r, kSessionUnconfirmed),
        "an unknown session identity refuses unsupported requests, naming the session");
  r.beginSessionTransition(120, true, "9999", "op");
  r.acceptLoginSession(120, 1, 7, "9999", "op");
  CHECK(unsupportedRefused(r, kSessionUnconfirmed),
        "a login without account confirmation still refuses unsupported requests");
  r.confirmSessionAccount(1, 7, "9999", "inv");
  CHECK(unsupportedAdmitted(r),
        "a confirmed session with no local rule admits unsupported requests");
  struct Rule {
    void (*apply)(RiskLimits &);
    const char *label;
  };
  const Rule rules[] = {
      {[](RiskLimits &c) { c.maxOrderVolume = 1; },
       "maxOrderVolume makes unsupported requests fail closed"},
      {[](RiskLimits &c) { c.maxPriceDeviation = 0.01; },
       "maxPriceDeviation makes unsupported requests fail closed"},
      {[](RiskLimits &c) { c.maxNotional = 1; },
       "maxNotional makes unsupported requests fail closed"},
      {[](RiskLimits &c) { c.maxOrdersPerSec = 1; },
       "an opening-order rate makes unsupported requests fail closed"},
      {[](RiskLimits &c) { c.maxCloseOrdersPerSec = 1; },
       "a closing-order rate makes unsupported requests fail closed"},
      {[](RiskLimits &c) { c.maxReferenceAgeMs = 0; },
       "an invalid configuration makes unsupported requests fail closed"},
  };
  for (const auto &rule : rules) {
    RiskLimits c;
    rule.apply(c);
    r.configure(c);
    CHECK(unsupportedRefused(r, kUnsupportedControls), rule.label);
    r.configure(RiskLimits{});
  }
  RiskLimits inert;
  inert.orderBurst = 5;
  inert.closeOrderBurst = 5;
  inert.maxReferenceAgeMs = 500;
  r.configure(inert);
  CHECK(unsupportedAdmitted(r),
        "bursts without rates and a reference window alone enable no local rule");
}

int main() {
  testRateLimiter();
  testConfigure();
  testCheck();
  testReferencePrice();
  testInstrumentMetadata();
  testModesAndSendBarrier();
  testLoginLifecycle();
  testAccountConfirmation();
  testSessionSeedingAndInvalidation();
  testAccountScope();
  testUnsupportedRequests();
  REPORT("RISK-CPP TEST");
}
