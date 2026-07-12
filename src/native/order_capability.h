/*
 * order_capability.h - the deliberately narrow ordinary-order capability.
 *
 * Local reservation/margin accounting models one single-instrument, immediate
 * GFD limit order in the confirmed InvestorID.  Every other InputOrder mode is
 * rejected before it reaches CTP; accepting it as an "ordinary" order would
 * silently claim risk coverage for dimensions the ledger does not represent.
 */
#ifndef CTP_NATIVE_ORDER_CAPABILITY_H
#define CTP_NATIVE_ORDER_CAPABILITY_H

#include "ThostFtdcUserApiStruct.h"
#include "ctp_field.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace ctp {

struct OrdinaryOrderIdentity {
  std::string brokerId;
  std::string investorId;
  std::string orderRef;
  std::string instrumentId;
};

inline bool decodeOrdinaryOrder(
    const CThostFtdcInputOrderField &f, OrdinaryOrderIdentity &identity) {
  std::string userId, gtdDate, businessUnit, exchangeId, investUnitId;
  std::string accountId, currencyId, clientId, macAddress, ipAddress, memo;
  if (!boundedCtpField(f.BrokerID, identity.brokerId) ||
      !boundedCtpField(f.InvestorID, identity.investorId) ||
      !boundedCtpField(f.OrderRef, identity.orderRef) ||
      !boundedCtpField(f.UserID, userId) ||
      !boundedCtpField(f.GTDDate, gtdDate) ||
      !boundedCtpField(f.BusinessUnit, businessUnit) ||
      !boundedCtpField(f.ExchangeID, exchangeId) ||
      !boundedCtpField(f.InvestUnitID, investUnitId) ||
      !boundedCtpField(f.AccountID, accountId) ||
      !boundedCtpField(f.CurrencyID, currencyId) ||
      !boundedCtpField(f.ClientID, clientId) ||
      !boundedCtpField(f.MacAddress, macAddress) ||
      !boundedCtpField(f.InstrumentID, identity.instrumentId) ||
      !boundedCtpField(f.IPAddress, ipAddress) ||
      !boundedCtpField(f.OrderMemo, memo))
    return false;

  const bool validHedge = f.CombHedgeFlag[0] == '1' ||
                          f.CombHedgeFlag[0] == '2' ||
                          f.CombHedgeFlag[0] == '3' ||
                          f.CombHedgeFlag[0] == '5' ||
                          f.CombHedgeFlag[0] == '6' ||
                          f.CombHedgeFlag[0] == '7';
  return !identity.brokerId.empty() && !identity.investorId.empty() &&
         !identity.orderRef.empty() && !identity.instrumentId.empty() &&
         (f.Direction == '0' || f.Direction == '1') &&
         f.CombOffsetFlag[0] >= '0' && f.CombOffsetFlag[0] <= '6' &&
         f.CombOffsetFlag[1] == '\0' && validHedge &&
         f.CombHedgeFlag[1] == '\0' && f.OrderPriceType == '2' &&
         std::isfinite(f.LimitPrice) && f.LimitPrice > 0.0 &&
         f.LimitPrice < 1e300 && f.VolumeTotalOriginal > 0 &&
         f.TimeCondition == '3' && gtdDate.empty() &&
         f.VolumeCondition == '1' && f.MinVolume == 1 &&
         f.ContingentCondition == '1' && f.StopPrice == 0.0 &&
         f.ForceCloseReason == '0' && f.IsAutoSuspend == 0 &&
         businessUnit.empty() && f.RequestID == 0 &&
         f.UserForceClose == 0 && f.IsSwapOrder == 0 &&
         exchangeId.empty() && investUnitId.empty() && accountId.empty() &&
         currencyId.empty() && clientId.empty() && f.SessionReqSeq == 0;
}

} // namespace ctp

#endif /* CTP_NATIVE_ORDER_CAPABILITY_H */
