/*
 * risk.h - stateless pre-trade risk enforcement + rate limiting, sunk into C++.
 *
 * Every rule here judges one order from the order itself, the latest market
 * reference and static instrument metadata. Nothing depends on what the account
 * holds or has working, so there is no position/order ledger to rebuild after a
 * reconnect and no state that can disagree with the caller's own books.
 * Account-level exposure caps belong to the trading layer that owns the account
 * facts. Config is held in atomics, published from the slow path (JS) and read
 * on the order path right before the vendor call.
 */

#ifndef CTP_NATIVE_RISK_H
#define CTP_NATIVE_RISK_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ctp_field.h"

namespace ctp {

using RiskSendMutex = std::recursive_mutex;

// The identifying fields shared by a ReqQryInstrument filter and an
// OnRspQryInstrument row. An empty filter field does not restrict the query.
struct InstrumentIdentity {
  std::string instrumentId;
  std::string exchangeId;
  std::string productId;
  std::string exchangeInstId;
};

// Read the identity of a CTP instrument filter or row; false when any field is
// not NUL-terminated within its width (the row or filter is unreadable).
template <typename Field>
inline bool readInstrumentIdentity(const Field &f, InstrumentIdentity &out) {
  return boundedCtpField(f.InstrumentID, out.instrumentId) &&
         boundedCtpField(f.ExchangeID, out.exchangeId) &&
         boundedCtpField(f.ProductID, out.productId) &&
         boundedCtpField(f.ExchangeInstID, out.exchangeInstId);
}

// A read-only source of latest reference prices (the MarketData snapshot cache
// implements it). Lets the risk engine read live prices for the deviation check
// in C++; attached via setMdSnapshot with no dependency on the CTP md struct
// and no JS round-trip.
struct RefPriceSource {
  // Atomically pair a price with its age.
  virtual bool quote(const std::string &instrumentId, double &price,
                     uint64_t &age) const = 0;
  virtual ~RefPriceSource() = default;
};

struct RiskLimits {
  int maxOrderVolume = 0;          // per single order; 0 = disabled
  double maxPriceDeviation = 0.0;  // |px-ref|/ref ratio; 0 = disabled
  double maxNotional = 0.0;        // price * volume * multiplier; 0 = disabled
  double maxOrdersPerSec = 0.0;    // opening-order token-bucket rate; 0 = off
  double orderBurst = 0.0;         // bucket burst; <=0 -> defaults to rate
  double maxCloseOrdersPerSec = 0.0; // independent reduce-order rate; 0 = off
  double closeOrderBurst = 0.0;
  double maxReferenceAgeMs = 3000; // reference quote freshness; must be > 0
};

enum class RiskMode : uint8_t {
  Normal,
  ReduceOnly,
  HaltAll,
};

struct RiskVerdict {
  bool ok = true;
  const char *reason = nullptr;    // static string; null when ok
};

// Token-bucket rate limiter. Thread-safe via a tiny mutex; rate<=0 disables.
class RateLimiter {
public:
  void configure(double ratePerSec, double burst);
  bool tryAcquire();               // true = allowed, false = throttled
  bool enabled() const;

private:
  mutable std::mutex m_;
  double rate_ = 0.0;
  double burst_ = 0.0;
  double tokens_ = 0.0;
  std::chrono::steady_clock::time_point last_{};
};

class RiskEngine {
public:
  void configure(const RiskLimits &cfg); // slow path (JS thread)
  void reduceOnly(); // block risk-increasing requests, keep reductions/cancels
  void haltAll();    // block both opening and closing order inserts
  void resume();     // Normal
  RiskMode mode() const { return mode_.load(std::memory_order_acquire); }

  // Validation only. Opening orders fail closed when an enabled deviation or
  // notional rule lacks a usable reference or multiplier.
  RiskVerdict check(const std::string &instrumentId, double price,
                    double refPrice, int volume, bool isOpen = true) const;
  // Static reason string of the most recent rejection (for diagnostics /
  // JS-side logging); null if none. Cheap, set only on the reject path.
  const char *lastBlockReason() const { return lastReason_; }

  // Rate gate (consumes a token). Call right before sending.
  bool allowRate() { return limiter_.tryAcquire(); }
  bool allowCloseRate() { return closeLimiter_.tryAcquire(); }
  // RAII lock spanning every gate and the vendor call.
  // `allowed` is evaluated under the same barrier used by configure/mode changes.
  std::unique_lock<RiskSendMutex> acquireOrderSend(bool isOpen, bool &allowed);
  // Ordinary orders additionally bind the final send barrier to the confirmed
  // BrokerID/InvestorID. Session transitions take the same barrier, so an order
  // cannot pass an account check and then be sent in a newly logged-in account.
  std::unique_lock<RiskSendMutex> acquireOrderSendForAccount(
      bool isOpen, const std::string &brokerId,
      const std::string &investorId, bool &allowed);

  // Attach a live reference-price source (the MarketData snapshot cache) so the
  // maxPriceDeviation / maxNotional checks read current prices in C++ straight
  // from the MD feed, no JS round-trip. Pass nullptr to detach. Without one,
  // refPrice() returns 0 and an enabled reference rule rejects opening orders.
  void setMdSnapshot(std::shared_ptr<const RefPriceSource> source);
  // Reference price: the attached MD snapshot's last price, or 0 if
  // none/unknown/older than maxReferenceAgeMs.
  double refPrice(const std::string &instrumentId) const;

  // Instrument metadata from reqQryInstrument, published per query. Only
  // futures/TAS carry the futures notional formula; other product classes (e.g.
  // options) fail closed for the maxNotional cap, because a futures notional
  // would misrepresent, say, an option premium. The multiplier is still
  // recorded for every class.
  //
  // beginInstrumentQuery runs immediately before ReqQryInstrument is sent (a
  // refused send is then finished as failed): a query naming an InstrumentID
  // clears that instrument and an unfiltered query clears every instrument, so
  // old values are unusable while the refresh is in flight. A query filtered
  // only by exchange, product or exchange instrument code clears nothing and
  // its publication merges: it cannot vouch for, nor wipe, the instruments
  // outside its scope. Each query opens the staging area of exactly this
  // request id, superseding any earlier query. Rows of the current query are
  // only staged; the successful final packet (finishInstrumentQuery(id, true))
  // publishes them together. An error packet, a refused send or the generic
  // error callback of this id (finishInstrumentQuery(id, false)), a front
  // transition or any JS-side rejection of all in-flight requests
  // (abandonInstrumentQuery), a JS idle timeout (timeoutInstrumentQuery) or a
  // newer query discards them, and rows or final packets carrying any other
  // request id are ignored.
  void beginInstrumentQuery(int requestId, const InstrumentIdentity &scope);
  // Stage one row. A row outside the query's scope (any non-empty filter field
  // it does not match) is not staged. Rows of one instrument must agree on the
  // exchange, product class and multiplier, and an out-of-scope row carrying a
  // staged InstrumentID counts as a disagreement; any disagreement, or an
  // unusable multiplier, clears that instrument on publication. The moment a
  // disagreement is staged it marks the InstrumentID ambiguous and voids its
  // published multiplier, and the mark stays however the query ends
  // (published, failed, timed out, abandoned or superseded), so even a
  // filtered query that keeps old values leaves none readable for it. Only a
  // successful, readable query without exchange/product/code filters (the
  // whole catalog, or that InstrumentID) whose answer carries consistent rows
  // of it lifts the mark; a failed, empty or unreadable answer never does.
  // While marked, the InstrumentID is published by no filtered query.
  void stageInstrument(int requestId, const InstrumentIdentity &row,
                       double multiplier, char productClass);
  // A row (or filter) whose identity cannot be read: its successful query
  // publishes nothing and clears every instrument, even for a filtered query.
  void stageUnreadableInstrument(int requestId);
  void finishInstrumentQuery(int requestId, bool succeeded);
  // Discards the staging of the current query (a front transition, or JS
  // rejecting every in-flight request). The id of a query whose final packet
  // already ran is kept for timeoutInstrumentQuery; that publication stands.
  void abandonInstrumentQuery();
  // JS idle-timeout linearization, as for session transitions: true means this
  // exact query was still staging and is now discarded, so its rows can never
  // publish; false means its final packet already published or discarded them
  // and the queued lossless response should settle the Promise.
  bool timeoutInstrumentQuery(int requestId);
  // The instruments (and multipliers) the query with this request id published
  // when its final packet ran: false, with nothing, for a request that is not
  // the latest finished query; true with an empty list for a failed or
  // unreadable answer. Read while JS settles that request, so no later query
  // can interfere.
  bool instrumentQueryPublication(
      int requestId, std::vector<std::pair<std::string, double>> &published) const;

  // Our login identity. A front transition invalidates it; account-bound order
  // inserts remain fail-closed until a concrete login response supplies the new
  // identity and the high-level handshake confirms the trading account.
  void invalidateSession();
  // Correlate login/logout responses with the most recent explicit request.
  // Trader request ids are echoed reliably, unlike MD id-0 responses. A newer
  // transition supersedes the old id so its delayed success cannot restore an
  // obsolete native session identity.
  void beginSessionTransition(int requestId, bool expectsLogin,
                              const std::string &brokerId = "",
                              const std::string &userId = "");
  bool acceptLoginSession(int requestId, int frontId, int sessionId,
                          const std::string &brokerId,
                          const std::string &userId);
  // RspUserLogin identifies the operator UserID, not necessarily the trading
  // InvestorID. The high-level session handshake explicitly confirms the
  // account used by orders before account-bound sends are accepted.
  bool confirmSessionAccount(int frontId, int sessionId,
                             const std::string &brokerId,
                             const std::string &investorId);
  bool acceptsOrderAccount(const std::string &brokerId,
                           const std::string &investorId) const;
  void finishSessionTransition(int requestId);
  // JS idle-timeout linearization. True means this exact request was still
  // pending and is now revoked; false means its final native callback already
  // won and the queued lossless response should settle the Promise.
  bool timeoutSessionTransition(int requestId);
  // Standalone (unit-test) seeding of a confirmed identity.
  void setSession(int frontId, int sessionId,
                  const std::string &brokerId = "",
                  const std::string &investorId = "");

  // Requests outside the ordinary InputOrder path (complex inserts,
  // hedge/offset confirmations, etc.) cannot be judged by these rules. They
  // pass only in Normal mode, for a known session, with no local rule enabled.
  // A refusal records its cause in lastBlockReason(); a pass clears it.
  bool allowUnsupportedRiskRequest() const;

private:
  enum class AccountScope : uint8_t { Current, Foreign, Ambiguous };
  AccountScope accountScopeLocked(const std::string &brokerId,
                                  const std::string &investorId) const;
  bool localControlsEnabled() const;
  // Record + return a rejection (stores the reason for lastBlockReason).
  RiskVerdict block(const char *reason) const;

  std::atomic<RiskMode> mode_{RiskMode::Normal};
  // Public N-API calls are validated, but direct/native callers must fail
  // closed too. A valid complete configure() call clears this state.
  std::atomic<bool> invalidConfig_{false};
  // Recursive so a vendor implementation that invokes an SPI callback inline
  // from ReqOrderInsert cannot deadlock when that callback takes the barrier.
  mutable RiskSendMutex sendModeMutex_;
  std::atomic<int> maxOrderVolume_{0};
  std::atomic<double> maxPriceDeviation_{0.0};
  std::atomic<double> maxNotional_{0.0};
  std::atomic<double> maxReferenceAgeMs_{3000.0};
  RateLimiter limiter_;
  RateLimiter closeLimiter_;
  // Live reference-price source (the MarketData snapshot cache), read by
  // refPrice(). Guarded by refMutex_.
  mutable std::mutex refMutex_;
  std::shared_ptr<const RefPriceSource> mdSnap_;

  // Guards instrument metadata and the session identity below.
  mutable std::mutex stateMutex_;
  struct InstrumentMeta {
    double multiplier = 0.0;
    bool futuresNotional = false; // futures/TAS: price * volume * multiplier
  };
  std::unordered_map<std::string, InstrumentMeta> instruments_;
  // The one instrument query whose rows may be staged (0 = none).
  int instrumentQueryId_ = 0;
  // The latest query whose final packet ran natively; its response may still
  // be queued for JS.
  int finishedInstrumentQueryId_ = 0;
  // What that query published: InstrumentID and multiplier (no particular order).
  std::vector<std::pair<std::string, double>> finishedPublished_;
  bool instrumentQueryUnreadable_ = false;
  InstrumentIdentity instrumentQueryScope_;
  // InstrumentIDs seen outside the current query's scope.
  std::unordered_set<std::string> outOfScopeInstruments_;
  // InstrumentIDs whose identity a query found ambiguous (see stageInstrument).
  std::unordered_set<std::string> ambiguousInstruments_;
  struct StagedInstrument {
    InstrumentMeta meta;
    std::string exchangeId;
    char productClass = 0;
    double multiplier = 0.0;
    bool conflicted = false;
  };
  // multiplier 0 marks an instrument the query clears on publication.
  std::unordered_map<std::string, StagedInstrument> stagedInstruments_;
  void resetInstrumentQueryLocked();
  void markAmbiguousLocked(const std::string &instrumentId);
  int myFrontId_ = 0, mySessionId_ = 0;
  std::string myBrokerId_, myInvestorId_;
  // Standalone RiskEngine unit users can seed a session directly. Once a real
  // front lifecycle is observed, however, a fresh login identity is mandatory.
  // The N-API Trader requires it from construction (invalidateSession() in its
  // constructor), so nothing passes before the first confirmed session.
  bool sessionIdentityRequired_ = false;
  bool sessionIdentityKnown_ = false;
  int pendingSessionRequestId_ = 0;
  bool pendingSessionExpectsLogin_ = false;
  std::string pendingSessionBrokerId_, pendingSessionUserId_;
  bool loginSessionKnown_ = false;

  // Per-calling-thread diagnostic: a rejection on one thread cannot overwrite
  // the reason a simultaneous send on another thread is about to surface.
  inline static thread_local const char *lastReason_ = nullptr;
};

} // namespace ctp

#endif /* CTP_NATIVE_RISK_H */
