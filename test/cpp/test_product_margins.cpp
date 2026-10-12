#include "risk.h"
#include "check.h"
#include <limits>
using namespace ctp;
struct Prices : RefPriceSource {
  bool quote(const std::string &, double &price, uint64_t &age) const override {
    price = 100; age = 0; return true;
  }
};
static void ready(RiskEngine &r) {
  r.setSession(1, 7, "9999", "u");
  r.setMdSnapshot(std::make_shared<Prices>());
  r.setInstrument("rb2610", 10, '1', "rb");
  r.setInstrument("rb2701", 10, '1', "rb");
  r.setInstrument("rb2710", 10, '1', "rb");
  r.setInstrument("hc2610", 10, '1', "hc");
  r.replacePositionsIfRevision(r.revision(), {});
  r.rebuildOpenReservationsIfRevision(r.revision(), {});
}
static OpenGate gate(RiskEngine &r, const std::string &id, double cap, char hedge = '1') {
  RiskLimits config; config.maxAccountMargin = cap; r.configure(config);
  r.rebuildOpenReservationsIfRevision(r.revision(), {});
  static int ref = 0;
  return r.tryReserveOpen(std::to_string(++ref), id, true, 100, 1, hedge);
}
static void row(RiskEngine &r, int req, const std::string &id, double rate, bool relative = false, char hedge = '1') {
  r.applyInvestorMarginSchedule(req, "9999", "u", "", id, hedge, rate, 0, rate, 0, relative);
}
int main() {
  RiskEngine r; ready(r);
  r.beginMarginScheduleQuery(10, false, "rb", '1', "9999", "u");
  row(r, 10, "rb2701", .2); row(r, 10, "rb", .1);
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "partial product response cannot open");
  r.finishMarginScheduleQuery(10, true);
  CHECK(gate(r, "rb2610", 150) == OpenGate::Ok, "product schedule expands to ordinary month");
  CHECK(gate(r, "rb2701", 150) != OpenGate::Ok, "contract override wins independent of row order");
  CHECK(gate(r, "rb2701", 250) == OpenGate::Ok, "override retains its actual cost");
  CHECK(gate(r, "hc2610", 150) == OpenGate::MetadataUnavailable, "product query cannot cover unrelated product");
  CHECK(gate(r, "rb2610", 150, '2') == OpenGate::MetadataUnavailable, "hedge flags remain isolated");
  r.beginMarginScheduleQuery(11, false, "rb", '1', "9999", "u");
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "refresh invalidates all product members");
  row(r, 11, "rb", .1); row(r, 11, "rb2701", std::numeric_limits<double>::max());
  r.finishMarginScheduleQuery(11, true);
  CHECK(gate(r, "rb2701", 10000) == OpenGate::MetadataUnavailable, "invalid explicit override never inherits product rate");
  CHECK(gate(r, "rb2610", 150) == OpenGate::Ok, "other valid member remains available");
  r.beginMarginScheduleQuery(12, false, "rb", '1', "9999", "u");
  row(r, 12, "rb", .1); r.finishMarginScheduleQuery(12, false);
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "failed query cannot restore old product rates");
  r.beginMarginScheduleQuery(13, false, "rb", '1', "9999", "u");
  row(r, 12, "rb", .1);
  r.applyInvestorMarginSchedule(13, "9999", "other", "", "rb", '1', .1, 0, .1, 0, false);
  r.finishMarginScheduleQuery(13, true);
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "late and foreign-account rows are rejected");
  r.beginMarginScheduleQuery(14, false, "rb", '1', "9999", "u");
  row(r, 14, "rb", .02, true); r.finishMarginScheduleQuery(14, true);
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "relative rate requires exchange base");
  r.beginMarginScheduleQuery(15, true, "rb2610", '1', "9999", "");
  r.applyExchangeMarginSchedule(15, "9999", "rb2610", '1', .1, 0, .1, 0);
  r.finishMarginScheduleQuery(15, true);
  CHECK(gate(r, "rb2610", 110) != OpenGate::Ok && gate(r, "rb2610", 125) == OpenGate::Ok, "relative and exchange components are added");
  r.beginMarginScheduleQuery(16, false, "rb", '1', "9999", "u");
  row(r, 16, "rb", .02, true); r.finishMarginScheduleQuery(16, true);
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "new investor generation invalidates prior exchange base");
  r.beginMarginScheduleQuery(160, true, "rb", '1', "9999", "");
  r.applyExchangeMarginSchedule(160, "9999", "rb", '1', .1, 0, .1, 0);
  r.applyExchangeMarginSchedule(160, "9999", "rb2701", '1', .2, 0, .2, 0);
  r.applyExchangeMarginSchedule(160, "9999", "rb2710", '1', -1, 0, .2, 0);
  r.finishMarginScheduleQuery(160, true);
  CHECK(gate(r, "rb2610", 125) == OpenGate::Ok && gate(r, "rb2701", 150) != OpenGate::Ok,
        "exchange product defaults and contract overrides remain separate");
  CHECK(gate(r, "rb2710", 10000) == OpenGate::MetadataUnavailable,
        "invalid exchange override cannot inherit a valid base");
  r.beginMarginScheduleQuery(17, false, "rb", '\0', "9999", "u");
  row(r, 17, "rb", .1, false, '2'); row(r, 17, "rb2701", .2, false, '2');
  r.finishMarginScheduleQuery(17, true);
  CHECK(gate(r, "rb2610", 150, '2') == OpenGate::Ok, "wildcard hedge query expands each returned hedge independently");
  r.invalidateSession(); row(r, 17, "rb", .1); r.finishMarginScheduleQuery(17, true);
  ready(r);
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "reconnect never reuses previous rates");
  r.invalidateInstrument("rb2610");
  r.beginMarginScheduleQuery(18, false, "rb", '1', "9999", "u");
  row(r, 18, "rb", .1); r.finishMarginScheduleQuery(18, true);
  r.setInstrument("rb2610", 10, '1', "hc");
  CHECK(gate(r, "rb2610", 150) == OpenGate::MetadataUnavailable, "removed catalog membership cannot receive a product rate");
  REPORT("product margins");
}
