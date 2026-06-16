/*
 * test_arm.cpp - standalone unit tests for ArmRegistry (src/native/arm.cc).
 *
 * Links arm.cc directly and drives onTick() with a mock OrderSink (no N-API, no
 * MarketData, no shipped test hooks). Covers the trigger logic (buy: ask<=trigger,
 * sell: bid>=trigger), one-shot semantics, the DBL_MAX no-bid guard, the fired vs
 * blocked counters, the "no arms" fast gate, and clearSink teardown safety.
 */
#include "arm.h"

#include <cstdint>
#include <limits>
#include <string>

#include "check.h"

using namespace ctp;
static const double DMAX = std::numeric_limits<double>::max(); // limit-down "no bid" sentinel

// Records fires; returns a configurable rc (0 = sent, non-zero = refused by risk/send).
struct MockSink : OrderSink {
  int rc = 0;
  int fires = 0;
  std::string lastInstrument;
  int fireArmed(const ArmSpec &spec) override {
    fires++;
    lastInstrument = spec.instrumentId;
    return rc;
  }
};

static ArmSpec mkSpec(const char *instr, char side, double trigger) {
  ArmSpec s;
  s.instrumentId = instr;
  s.side = side;
  s.triggerPrice = trigger;
  s.orderTemplate.resize(16, 0); // dummy pre-encoded template
  return s;
}

int main() {
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    uint64_t id = reg.arm(mkSpec("rb", '0', 3500));
    CHECK(reg.size() == 1, "arm() adds a trigger");
    CHECK(reg.disarm(id), "disarm() returns true for a live id");
    CHECK(reg.size() == 0, "disarm() removes the trigger");
    CHECK(!reg.disarm(999999), "disarm() of an unknown id returns false");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '0', 3500)); // buy
    reg.onTick("rb", 3490, 3501);     // ask 3501 > 3500 -> no fire
    CHECK(sink.fires == 0, "buy: ask above the trigger does not fire");
    reg.onTick("rb", 3490, 3499); // ask 3499 <= 3500 -> fire
    CHECK(sink.fires == 1, "buy: ask at/below the trigger fires");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '1', 3500)); // sell
    reg.onTick("rb", 3499, 3502);     // bid 3499 < 3500 -> no fire
    CHECK(sink.fires == 0, "sell: bid below the trigger does not fire");
    reg.onTick("rb", 3501, 3502); // bid 3501 >= 3500 -> fire
    CHECK(sink.fires == 1, "sell: bid at/above the trigger fires");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '0', 3500));
    reg.onTick("rb", 3490, 3499); // fire
    reg.onTick("rb", 3490, 3499); // already fired -> must not re-fire
    CHECK(sink.fires == 1, "one-shot: an armed trigger fires at most once");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '1', 3500)); // sell fires on bid >= trigger
    reg.onTick("rb", DMAX, 3502);     // bid = DBL_MAX (limit-down, no real bid)
    CHECK(sink.fires == 0, "sell: a DBL_MAX no-bid sentinel does not fire (guarded)");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '0', 3500));
    reg.onTick("ag", 1, 1); // a different instrument
    CHECK(sink.fires == 0, "onTick for a different instrument does not fire");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    sink.rc = 0; // sink accepts
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '0', 3500));
    reg.onTick("rb", 3490, 3499);
    CHECK(reg.fireCount() == 1 && reg.blockedCount() == 0, "fireCount increments when the sink accepts (rc 0)");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    sink.rc = -1; // sink refuses (risk gate / send error)
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '0', 3500));
    reg.onTick("rb", 3490, 3499);
    CHECK(reg.blockedCount() == 1 && reg.fireCount() == 0, "blockedCount increments when the sink refuses (rc != 0)");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.onTick("rb", 3490, 3499); // no arms registered
    CHECK(sink.fires == 0, "onTick with no arms is a harmless no-op (fast gate)");
  }
  {
    ArmRegistry reg;
    MockSink sink;
    reg.setSink(&sink);
    reg.arm(mkSpec("rb", '0', 3500));
    reg.clearSink();              // teardown
    reg.onTick("rb", 3490, 3499); // a hit after the sink is cleared
    CHECK(sink.fires == 0, "clearSink: a trigger hit after teardown does not fire");
  }

  REPORT("ARM-CPP TEST");
}
