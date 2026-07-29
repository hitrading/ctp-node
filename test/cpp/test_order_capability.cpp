/*
 * Verifies the exact ordinary-order policies admitted by both the native
 * pre-trade gate and the reservation ledger contract.
 */
#include "order_capability.h"

#include <cstdio>
#include <cstring>

#include "check.h"

using namespace ctp;

template <std::size_t N>
static void setField(char (&target)[N], const char *value) {
  std::snprintf(target, N, "%s", value);
}

static CThostFtdcInputOrderField ordinaryOrder(char timeCondition,
                                               char volumeCondition) {
  CThostFtdcInputOrderField order{};
  setField(order.BrokerID, "9999");
  setField(order.InvestorID, "investor");
  setField(order.OrderRef, "1");
  setField(order.InstrumentID, "rb2610");
  order.Direction = '0';
  order.CombOffsetFlag[0] = '0';
  order.CombHedgeFlag[0] = '1';
  order.OrderPriceType = '2';
  order.LimitPrice = 3500.0;
  order.VolumeTotalOriginal = 1;
  order.TimeCondition = timeCondition;
  order.VolumeCondition = volumeCondition;
  order.MinVolume = 1;
  order.ContingentCondition = '1';
  order.ForceCloseReason = '0';
  return order;
}

static bool accepted(char timeCondition, char volumeCondition) {
  OrdinaryOrderIdentity identity;
  const auto order = ordinaryOrder(timeCondition, volumeCondition);
  return decodeOrdinaryOrder(order, identity) && identity.brokerId == "9999" &&
         identity.investorId == "investor" && identity.orderRef == "1" &&
         identity.instrumentId == "rb2610";
}

int main() {
  CHECK(accepted('3', '1'), "GFD limit orders are admitted");
  CHECK(accepted('1', '1'), "FAK limit orders are admitted");
  CHECK(accepted('1', '3'), "FOK limit orders are admitted");
  CHECK(!accepted('3', '3'), "GFD plus complete-volume is rejected");
  CHECK(!accepted('1', '2'), "minimum-volume IOC is rejected");
  CHECK(!accepted('5', '1'), "unsupported time conditions are rejected");
  REPORT("order capability");
}
