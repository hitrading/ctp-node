#include "risk.h"
#include <iostream>

using namespace ctp;

int main() {
  int failures = 0;
  auto check = [&](bool ok, const char *label) {
    if (!ok) { std::cerr << label << '\n'; ++failures; }
  };
  for (int i = 0; i < 3; ++i) {
    RiskEngine r;
    r.setSession(1, 7, "9999", "investor");
    r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
    check(r.replacePositionsIfRevision(r.revision(), {{"rb", true, 2, 300, '1'}}), "seed");
    const auto rev = r.revision();
    r.onTrade("20261124|fill", "1", "sys", i == 2 ? " rb" : "rb", true, true, '1', 3500, 1,
              i == 0 ? "9999 " : "9999", i == 1 ? " investor" : "investor");
    check(r.accountMargin() == 300, "preserve last reported margin");
    check(!r.replacePositionsIfRevision(rev, {}), "stale query after ambiguous fill");
    check(r.tryReserveOpen("blocked", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable,
          "ambiguous fill blocks opening");
    check(!r.replacePositionsIfRevision(r.revision(), {{"rb ", true, 2, 300, '1'}}), "padded snapshot");
    check(r.replacePositionsIfRevision(r.revision(), {{"rb", true, 3, 450, '1'}}), "reconcile");
    check(r.accountMargin() == 450, "publish new margin");
  }
  for (int i = 0; i < 2; ++i) {
    RiskEngine r;
    r.setSession(1, 7);
    r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
    const std::string sys = "20261124|SHFE|rb|123";
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 5, 0, sys);
    OpenOrderInfo row{2, 9, "1", "rb", i != 0, i == 0 ? '1' : '3', 5, 3500, 0, sys};
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "conflicting active row must not be dropped");
    check(r.tryReserveOpen("blocked", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable, "conflict blocks orders");
    r.onOrderUpdate(2, 9, "1", "rb", true, row.isLong, row.hedgeFlag, '5', 3500, 5, 0, sys);
    row.isLong = true;
    row.hedgeFlag = '1';
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "conflicting terminal evidence cannot release a reservation");
    check(r.rebuildOpenReservationsIfRevision(r.revision(), {}), "fresh empty snapshot");
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 5, 0, sys);
    check(r.tryReserveOpen("stale", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable,
          "conflicting terminal evidence cannot suppress a later live report");
    row.orderSysKey = "20261124|SHFE|rb|456";
    check(r.rebuildOpenReservationsIfRevision(r.revision(), {row, row}), "recover with independent exchange identity");
    row.sessionId = 10;
    row.orderSysKey += "b";
    r.onOrderUpdate(2, 10, "1", "rb", true, true, '1', '3', 3500, 5, 0, row.orderSysKey);
    r.onOrderUpdate(2, 10, "1", "rb", true, true, '1', '3', 3500, 5, 0, row.orderSysKey);
    r.setInstrumentPositionLimits({{"rb", true, 11, false, 0}});
    check(r.tryReserveOpen("one", "rb", true, 3500, 1) == OpenGate::Ok, "duplicate has no additional charge");
    check(r.tryReserveOpen("extra", "rb", true, 3500, 1) == OpenGate::VolumeLimit, "two sessions reserve ten lots");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    const std::string sys = "20261124|SHFE|rb|single";
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 5, 0, sys);
    OpenOrderInfo row{2, 9, "1", "rb", true, '1', 5, 3500, 0, sys, false};
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "unsupported working legs cannot match a single-leg terminal");
    r.onUnsupportedOrderUpdate(2, 9, "1", '5', sys);
    row.supportedSingleLeg = true;
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "unsupported terminal invalidates single-leg release evidence");
  }
  return failures != 0;
}
