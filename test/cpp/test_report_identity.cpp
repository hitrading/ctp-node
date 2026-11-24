#include "risk.h"
#include "ctp_field.h"
#include <iostream>

using namespace ctp;

int main() {
  int failures = 0;
  auto check = [&](bool ok, const char *label) {
    if (!ok) { std::cerr << label << '\n'; ++failures; }
  };
  {
    std::string id;
    const char unterminated[] = {'1', '2'};
    check(boundedExchangeId(" 123 ", id) && id == "123", "strip exchange padding");
    check(boundedExchangeId(" A B ", id) && id == "A B", "interior spaces are significant");
    check(boundedExchangeId("    ", id) && id.empty(), "blank exchange id is absent");
    check(!boundedExchangeId(unterminated, id), "unterminated exchange id is invalid");
    check(boundedCtpField("9999 ", id) && id == "9999 ", "account evidence stays byte-exact");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '1', 3500, 5, 2, "sys");
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 5, 1, "sys");
    check(r.tryReserveOpen("over", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable,
          "a terminal report cannot undo a previously acknowledged fill");
  }
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
  for (bool terminal : {false, true}) {
    RiskEngine r;
    r.setSession(1, 7);
    r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
    const std::string sys = "20261124|SHFE|rb|quantity";
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 5, 1, sys);
    const auto revision = r.revision();
    r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', terminal ? '5' : '1',
                    3500, 5, 2, sys);
    check(r.revision() > revision, "a report with more fills than the final count cannot be a replay");
    check(r.tryReserveOpen("over", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable,
          "contradictory traded volume blocks opening");
    OpenOrderInfo row{2, 9, "1", "rb", true, '1', 3, 3500, 2, sys};
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "conflicting snapshot fill count");
    row.vol = 5; row.traded = 1;
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "conflicting original size");
  }
  {
    RiskEngine r;
    r.setSession(1, 7);
    r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
    r.onOrderUpdate(3, 8, "1", "rb", true, true, '1', '1', 3000, 5, 1, "Q0");
    r.onOrderUpdate(3, 8, "1", "rb", true, true, '1', '5', 3000, 4, 1, "Q0");
    check(r.tryReserveOpen("blocked", "rb", true, 3000, 1) == OpenGate::MetadataUnavailable,
          "a terminal report for a different original size cannot release an active order");
    OpenOrderInfo row{3, 8, "1", "rb", true, '1', 4, 3000, 1, "Q0"};
    check(r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "known live order can reconcile after a malformed terminal");
    check(r.replacePositionsIfRevision(r.revision(), {}), "reconcile held positions after malformed terminal");
    r.setInstrumentPositionLimits({{"rb", true, 4, false, 0}});
    check(r.tryReserveOpen("blocked", "rb", true, 3000, 1) == OpenGate::VolumeLimit, "four working lots survive a malformed terminal");
  }
  for (int source = 0; source < 4; ++source) {
    RiskEngine r;
    r.setSession(1, 7, "9999", "investor");
    r.onOrderUpdate(3, -7, "1", "rb", true, true, '1', '5', 3000, 5, 1, "Q0", "9999", "investor");
    OpenOrderInfo row{3, -7, "1", "rb", true, '1', 3, 3000, 2, "Q0"};
    if (source < 2)
      r.onOrderUpdate(3, -7, "1", "rb", true, true, '1', source == 0 ? '1' : '5',
                      3000, 5, 2, "Q0", "9999", "investor");
    if (source == 3)
      r.onUnsupportedOrderUpdate(3, -7, "1", '3', "Q0", "9999", "investor");
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "conflicting same-system snapshot");
    row.traded = 1; row.vol = 4;
    check(!r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "later matching count cannot reinstate contradicted terminal");
    row.orderSysKey = "N0";
    check(r.rebuildOpenReservationsIfRevision(r.revision(), {row}), "independent system id restores four working lots");
    check(r.replacePositionsIfRevision(r.revision(), {}), "reconcile empty held positions");
    r.setInstrumentPositionLimits({{"rb", true, 4, false, 0}});
    check(r.tryReserveOpen("probe", "rb", true, 3000, 1) == OpenGate::VolumeLimit, "four working lots are occupied");
    r.setInstrumentPositionLimits({{"rb", true, 5, false, 0}});
    check(r.tryReserveOpen("probe", "rb", true, 3000, 1) == OpenGate::Ok, "exactly four working lots are occupied");
  }
  for (bool tradeFirst : {false, true}) {
    {
      RiskEngine r;
      r.setSession(1, 7);
      r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
      if (!tradeFirst)
        r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '3', 3500, 5, 0, "sys");
      r.onTrade("20261124|fill", "1", "sys", "rb", true, true, '1', 3500, 2);
      r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '5', 3500, 5, 1, "sys");
      check(r.tryReserveOpen("over", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable,
            "terminal cumulative volume cannot be less than the delivered fills");
    }
    for (int conflict = 0; conflict < 3; ++conflict) {
      RiskEngine r;
      r.setSession(1, 7);
      r.setInstrumentPositionLimits({{"rb", true, 10, false, 0}});
      const std::string sys = "20261124|SHFE|rb|arrival";
      auto trade = [&] {
        r.onTrade("20261124|fill", conflict == 0 ? "other" : "1", sys, "rb",
                  conflict != 1, true, conflict == 2 ? '3' : '1', 3500, 1);
      };
      if (tradeFirst) trade();
      r.onOrderUpdate(2, 9, "1", "rb", true, true, '1', '1', 3500, 5, 1, sys);
      if (!tradeFirst) trade();
      check(r.tryReserveOpen("over", "rb", true, 3500, 1) == OpenGate::MetadataUnavailable,
            "mismatched fills cannot release a working order in either callback order");
    }
  }
  return failures != 0;
}
