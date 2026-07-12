/*
 * risk.h - pre-trade risk enforcement + rate limiting, sunk into C++.
 *
 * These run on the HOT path (right before an order is sent - which may be the
 * JS thread today, or the CTP callback thread once armed triggers fire). They
 * must be deterministic and never blocked by a JS GC pause, so config is held
 * in atomics, published from the slow path (JS) and read lock-free on the hot
 * path. JS sets the rules; C++ enforces them on every order.
 */

#ifndef CTP_NATIVE_RISK_H
#define CTP_NATIVE_RISK_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ctp {

using RiskSendMutex = std::recursive_mutex;

// A read-only source of latest reference prices (the MarketData snapshot cache
// implements it). Lets the risk engine read live prices for the deviation check
// in C++; attached via setMdSnapshot with no dependency on the CTP md struct
// and no JS round-trip. last() returns 0 for an unknown/absent instrument.
struct RefPriceSource {
  // Atomically pair a price with its age.
  virtual bool quote(const std::string &instrumentId, double &price,
                     uint64_t &age) const = 0;
  virtual ~RefPriceSource() = default;
};

// One working open order, for rebuilding reservations from reqQryOrder.
struct OpenOrderInfo {
  int frontId = 0;
  int sessionId = 0;
  std::string orderRef;
  std::string instrumentId;
  bool isLong = true;
  char hedgeFlag = '1';
  double vol = 0.0;   // working remainder (VolumeTotalOriginal - VolumeTraded)
  double price = 0.0; // LimitPrice
  double traded = 0.0; // fills already reflected by the authoritative snapshot
  std::string orderSysKey; // ExchangeID|OrderSysID when assigned
  bool supportedSingleLeg = true;
};

struct PositionSeed {
  std::string instrumentId;
  bool isLong = true;
  double volume = 0.0;
  double margin = 0.0;
  char hedgeFlag = '1';
};

// One atomic per-instrument limit update. `updateLong` / `updateShort` allow a
// caller to leave one side unchanged; a zero limit removes that side's
// cap. A batch is validated before any entry is committed.
struct InstrumentPositionLimitUpdate {
  std::string instrumentId;
  bool updateLong = false;
  double longLimit = 0.0;
  bool updateShort = false;
  double shortLimit = 0.0;
};

struct InstrumentMarginLimitUpdate {
  std::string instrumentId;
  double limit = 0.0;
};

struct RiskLimits {
  int maxOrderVolume = 0;          // per single order; 0 = disabled
  double maxPriceDeviation = 0.0;  // |px-ref|/ref ratio; 0 = disabled
  double maxNotional = 0.0;        // price * volume * multiplier; 0 = disabled
  double maxOrdersPerSec = 0.0;    // opening-order token-bucket rate; 0 = off
  double orderBurst = 0.0;         // bucket burst; <=0 -> defaults to rate
  double maxCloseOrdersPerSec = 0.0; // independent reduce-order rate; 0 = off
  double closeOrderBurst = 0.0;
  double maxAccountMargin = 0.0;   // cap on total held+reserved margin; 0 = off
  double maxReferenceAgeMs = 3000; // deviation quote freshness; must be > 0
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

// Result of the position gate for an opening order.
enum class OpenGate {
  Ok,
  CostLimit,
  VolumeLimit,
  MetadataUnavailable,
  DuplicateOrderRef,
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
  bool initialized_ = false;
};

class RiskEngine {
public:
  void configure(const RiskLimits &cfg); // slow path (JS thread)
  void reduceOnly(); // block risk-increasing requests, keep reductions/cancels
  void haltAll();    // block both opening and closing order inserts
  void resume();     // Normal
  RiskMode mode() const { return mode_.load(std::memory_order_acquire); }

  // Validation only (no position side effects). Opening orders fail closed when
  // an enabled deviation/notional rule lacks a usable reference or multiplier.
  RiskVerdict check(const std::string &instrumentId, double price,
                    double refPrice, int volume, bool isOpen = true) const;
  // Static reason string of the most recent check() rejection (for diagnostics
  // / JS-side logging); null if none. Cheap, set only on the reject path.
  const char *lastBlockReason() const { return lastReason_; }

  // Rate gate (consumes a token). Call right before sending.
  bool allowRate() { return limiter_.tryAcquire(); }
  bool allowCloseRate() { return closeLimiter_.tryAcquire(); }
  // RAII lock spanning every gate and the vendor call.
  // `allowed` is evaluated under the same barrier used by configure/mode changes.
  std::unique_lock<RiskSendMutex> acquireOrderSend(bool isOpen, bool &allowed);
  // Ordinary orders additionally bind the final send barrier to the confirmed
  // BrokerID/InvestorID. Session transitions take the same send->position lock
  // order, so a close cannot pass an account check and then be sent in a newly
  // logged-in account (opens were already stopped later by reservation).
  std::unique_lock<RiskSendMutex> acquireOrderSendForAccount(
      bool isOpen, const std::string &brokerId,
      const std::string &investorId, bool &allowed);

  // Attach a live reference-price source (the MarketData snapshot cache) so the
  // maxPriceDeviation check reads current prices in C++ straight from the MD
  // feed, no JS round-trip. Pass nullptr to detach. Without one, refPrice()
  // returns 0 and an enabled deviation rule rejects opening orders.
  void setMdSnapshot(std::shared_ptr<const RefPriceSource> source);
  // Reference price for the deviation check: the attached MD snapshot's last
  // price, or 0 if none/unknown.
  double refPrice(const std::string &instrumentId) const;

  // Max total open-position MARGIN (sum open price*volume*multiplier*rate).
  // Updated by fills (onTrade) on the trader callback thread; authoritative
  // pre-existing positions enter through replacePositionsIfRevision(). Set the
  // contract multiplier AND margin rate for accuracy.
  // Instrument metadata from reqQryInstrument. Only futures/TAS carry the futures
  // by-money + by-volume margin formula; other product classes (e.g. options)
  // fail closed for BOTH margin reservations AND the maxNotional cap, because a
  // futures notional formula would misrepresent, say, an option premium. The
  // multiplier is still recorded for every class.
  void setInstrument(const std::string &instrumentId, double multiplier,
                     char productClass);
  // Called immediately before the corresponding CTP metadata query is sent.
  // Old values must not remain usable while a refresh is in flight or when the
  // refresh comes back empty/invalid.
  void invalidateInstrument(const std::string &instrumentId);
  // Per-instrument margin rate (fraction of notional, e.g. 0.10 = 10%), fed
  // directly from CTP's OnRspQryInstrumentMarginRate inside C++ (no JS round-
  // trip). New futures orders use the published schedule; an authoritative
  // position sync uses CTP `UseMargin`. This is a conservative local margin
  // ledger, not a universal replica of every exchange/broker calculation.
  // Unset/invalid schedules make enabled monetary caps fail closed.
  void setMarginSchedule(const std::string &instrumentId, char hedgeFlag,
                         double longByMoney, double longByVolume,
                         double shortByMoney, double shortByVolume,
                         bool isRelative);
  void setExchangeMarginSchedule(const std::string &instrumentId,
                                 char hedgeFlag, double longByMoney,
                                 double longByVolume, double shortByMoney,
                                 double shortByVolume);
  void invalidateMarginSchedules(const std::string &instrumentId,
                                 char hedgeFlag);
  void invalidateExchangeMarginSchedule(const std::string &instrumentId,
                                        char hedgeFlag);
  // Session/account-bound query pipeline. Only the newest request id in the
  // current authenticated account may publish a schedule; late rows from a
  // timed-out or previous-account query are ignored.
  void beginMarginScheduleQuery(int requestId, bool exchange,
                                const std::string &instrumentId,
                                char hedgeFlag,
                                const std::string &brokerId,
                                const std::string &investorId);
  void applyInvestorMarginSchedule(
      int requestId, const std::string &brokerId,
      const std::string &investorId, const std::string &investUnitId,
      const std::string &instrumentId, char hedgeFlag,
      double longByMoney, double longByVolume,
      double shortByMoney, double shortByVolume, bool isRelative);
  void applyExchangeMarginSchedule(
      int requestId, const std::string &brokerId,
      const std::string &instrumentId, char hedgeFlag,
      double longByMoney, double longByVolume,
      double shortByMoney, double shortByVolume);
  void finishMarginScheduleQuery(int requestId, bool success);
  // Generated SPI uses a stable exchange/trading-day TradeID key. Returns without
  // side effects when the same trade is replayed.
  void onTrade(const std::string &tradeKey, const std::string &orderRef,
               const std::string &orderSysKey,
               const std::string &instrumentId, bool isBuy, bool isOpen,
               char hedgeFlag, double price, double volume,
               const std::string &brokerId = "",
               const std::string &investorId = "");
  void markTradeLedgerUncertain();
  void markOrderLedgerUncertain();
  void resetPositions();
  bool replacePositionsIfRevision(uint64_t expected,
                                  const std::vector<PositionSeed> &positions);
  double accountMargin() const;
  // Apply a complete batch under the final send barrier. This prevents an order
  // from observing a half-updated pair of long/short limits or a partially
  // committed multi-instrument batch. Returns false without changing anything
  // when any entry is malformed.
  bool setInstrumentPositionLimits(
      const std::vector<InstrumentPositionLimitUpdate> &updates);
  // Per-instrument cap on scheduled open margin (by-money + by-volume, both
  // sides summed). 0/unset = no cap. Complements maxAccountMargin.
  bool setInstrumentMarginLimits(
      const std::vector<InstrumentMarginLimitUpdate> &updates);

  // Our login identity, set on rsp-user-login. A front transition invalidates
  // it and both ledgers; opening inserts remain fail-closed until a concrete
  // login response supplies the new identity. Reservations are keyed by
  // (front, session, orderRef) so orders from THIS session never collide with
  // another terminal's orders on the same account.
  void invalidateSession();
  // Correlate login/logout responses with the most recent explicit request.
  // Trader request ids are echoed reliably, unlike MD id-0 responses. A newer
  // transition supersedes the old id so its delayed success cannot restore an
  // obsolete native session identity.
  void beginSessionTransition(int requestId, bool expectsLogin,
                              const std::string &brokerId = "",
                              const std::string &investorId = "");
  bool acceptLoginSession(int requestId, int frontId, int sessionId,
                          const std::string &brokerId,
                          const std::string &userId);
  // RspUserLogin identifies the operator UserID, not necessarily the trading
  // InvestorID. The high-level session handshake explicitly confirms the
  // account used by orders/positions before native risk state becomes usable.
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
  void setSession(int frontId, int sessionId,
                  const std::string &brokerId = "",
                  const std::string &investorId = "");

  // Atomically check ALL position caps (per-side volume, per-instrument cost,
  // global cost) against committed = held + in-flight reserved, and, if the
  // order passes and any cap applies, reserve it (under our own session key) so
  // a burst of opens can't slip past before fills arrive. Returns the cap that
  // blocked, or Ok (reserved). Pair every Ok with bindInsertRequest before the
  // vendor call; callbacks reconcile via onOrderUpdate/rejectInsert*, while a
  // local/immediate send failure uses rollbackInsertRequest.
  OpenGate tryReserveOpen(const std::string &orderRef, const std::string &instrumentId,
                          bool isLong, double price, double volume,
                          char hedgeFlag = '1');
  // Reconcile an open order's reservation to its current working remainder
  // (volTotal - volTraded while queueing; 0 once terminal). Fed from OnRtnOrder,
  // which carries the originating session - so this also tracks (and releases)
  // orders placed by another terminal on the same account.
  void onOrderUpdate(int frontId, int sessionId, const std::string &orderRef,
                      const std::string &instrumentId, bool isOpen, bool isLong,
                      char hedgeFlag, char status, double limitPrice,
                      double volTotal, double volTraded,
                      const std::string &orderSysKey = "",
                      const std::string &brokerId = "",
                      const std::string &investorId = "");
  // A live multi-leg/otherwise unsupported working order cannot be represented
  // by the single-instrument ledger. Track its identity and fail closed for
  // all opening inserts until it becomes terminal or an
  // authoritative order snapshot confirms it is gone.
  void onUnsupportedOrderUpdate(int frontId, int sessionId,
                                const std::string &orderRef, char status,
                                const std::string &orderSysKey = "",
                                const std::string &brokerId = "",
                                const std::string &investorId = "");
  // Bind a successful reservation to the exact CTP request id before the
  // vendor call. The binding captures an internal generation token as well as
  // (front, session, OrderRef), so a delayed response can never release a newer
  // reservation after either session or OrderRef reuse. Returns false and
  // atomically rolls back the still-unbound reservation when the request id is
  // invalid/already live.
  bool bindInsertRequest(int requestId, const std::string &orderRef);
  // OnRspOrderInsert carries requestId and can therefore reject only the exact
  // bound reservation. Unknown, mismatched, or contradictory callbacks fail
  // closed instead of guessing the current session.
  void rejectInsertByRequest(int requestId, const std::string &orderRef,
                             const std::string &brokerId = "",
                             const std::string &investorId = "");
  // OnErrRtnOrderInsert has no requestId/front/session. It must never release by
  // OrderRef alone; a matching live reservation makes the order ledger
  // uncertain until an authoritative order snapshot reconciles it.
  void rejectInsertUnscoped(const std::string &orderRef,
                            const std::string &brokerId = "",
                            const std::string &investorId = "");
  // Undo the exact binding + reservation when CTP did not accept the request
  // (rate-limit or an immediate nonzero ReqOrderInsert return). The same
  // OrderRef then remains safe to retry.
  void rollbackInsertRequest(int requestId, const std::string &orderRef);
  // Rebuild all in-flight reservations from CTP's working orders (reqQryOrder).
  // Authoritative resync for (re)connect; clears prior reservations first.
  void rebuildOpenReservations(const std::vector<OpenOrderInfo> &orders);
  bool rebuildOpenReservationsIfRevision(uint64_t expected,
                                         const std::vector<OpenOrderInfo> &orders);

  uint64_t revision() const { return revision_.load(std::memory_order_acquire); }
  // Mutations not representable by the normal InputOrder ledger (complex
  // inserts, hedge/offset confirmations, etc.) may pass only when no local risk
  // control is configured; otherwise they fail closed.
  bool allowUnsupportedRiskRequest() const;
  // Once CTP accepts such a mutation, a later transition from protocol-binding
  // mode into locally risk-managed mode cannot assume the single-leg ledger is
  // complete. This history is intentionally sticky for the Trader lifetime.
  // The taint is unconditional (not account-scoped): CTP's private stream only
  // delivers the confirmed investor's realtime returns, so every such broadcast
  // is our own, and account text is not uniformly present anyway (bank-transfer
  // returns carry no InvestorID).
  void recordUnsupportedRiskMutation();

private:
  std::atomic<RiskMode> mode_{RiskMode::Normal};
  // Public N-API calls are validated, but direct/native callers must fail
  // closed too. A valid complete configure() call clears this state.
  std::atomic<bool> invalidConfig_{false};
  // Recursive so a vendor implementation that invokes an SPI callback inline
  // from ReqOrderInsert cannot deadlock when that callback updates the ledger.
  mutable RiskSendMutex sendModeMutex_;
  std::atomic<int> maxOrderVolume_{0};
  std::atomic<double> maxPriceDeviation_{0.0};
  std::atomic<double> maxNotional_{0.0};
  std::atomic<double> maxAccountMargin_{0.0};
  std::atomic<double> maxReferenceAgeMs_{3000.0};
  RateLimiter limiter_;
  RateLimiter closeLimiter_;
  // Live reference-price source (the MarketData snapshot cache), read by
  // refPrice() for the deviation check. Guarded by refMutex_.
  mutable std::mutex refMutex_;
  std::shared_ptr<const RefPriceSource> mdSnap_;

  struct Pos {
    struct HeldBucket {
      double vol = 0.0, cost = 0.0;
      bool costKnown = true;
    };
    double longVol = 0, longCost = 0, shortVol = 0, shortCost = 0;       // filled
    bool longCostKnown = true, shortCostKnown = true;
    std::unordered_map<char, HeldBucket> longHeld, shortHeld;
    double longPendVol = 0, longPendCost = 0, shortPendVol = 0, shortPendCost = 0; // in-flight reserved
    size_t longPendUnknown = 0, shortPendUnknown = 0;
  };
  mutable std::mutex posMutex_;
  std::unordered_map<std::string, Pos> positions_;
  int myFrontId_ = 0, mySessionId_ = 0; // our session, for reservation keys
  std::string myBrokerId_, myInvestorId_, myLoginUserId_;
  // Standalone RiskEngine unit users can seed a session directly. Once a real
  // front lifecycle is observed, however, a fresh login identity is mandatory.
  bool sessionIdentityRequired_ = false;
  bool sessionIdentityKnown_ = false;
  int pendingSessionRequestId_ = 0;
  bool pendingSessionExpectsLogin_ = false;
  std::string pendingSessionBrokerId_, pendingSessionUserId_;
  bool loginSessionKnown_ = false;
  // In-flight open-order reservations, keyed by (front:session:orderRef) so a
  // fill/cancel/reject releases exactly what each order reserved (its working
  // remainder), and our orders never collide with another terminal's.
  struct Reservation {
    std::string orderRef;
    std::string orderSysKey;
    std::string instrumentId;
    bool isLong = true;
    char hedgeFlag = '1';
    double vol = 0.0, cost = 0.0, price = 0.0;
    bool costKnown = false;
    int insertRequestId = 0;
    uint64_t insertBindingToken = 0;
  };
  std::unordered_map<std::string, Reservation> reservations_;
  enum class InsertRequestState : uint8_t {
    Pending,
    Observed,
    Rejected,
  };
  struct InsertRequestBinding {
    std::string reservationKey;
    std::string orderRef;
    uint64_t token = 0;
    InsertRequestState state = InsertRequestState::Pending;
  };
  // Exact insert-response correlation is intentionally retained after the
  // first callback, so duplicate/contradictory late responses cannot target a
  // newer reservation. The FIFO is bounded; an eventual callback for an
  // evicted id is treated as unknown and therefore fails closed.
  std::unordered_map<int, InsertRequestBinding> insertRequests_;
  std::deque<std::pair<int, uint64_t>> insertRequestOrder_;
  uint64_t nextInsertBindingToken_ = 1;
  static constexpr size_t kMaxInsertRequestBindings = 65536;
  struct OrderState {
    bool terminal = false;
    double remaining = 0.0;
    double reportedTraded = 0.0;
    double appliedTraded = 0.0;
    // Exchange/day/instrument/OrderSysID fingerprint. It distinguishes a known
    // stale replay from a genuinely new order after front/session/ref reuse.
    std::string orderSysKey;
  };
  // Tombstones make terminal state monotonic: an old non-terminal replay can no
  // longer resurrect a completed order. Exact tombstones are retained in a
  // bounded FIFO; evicting an exceptionally old tombstone can only recreate a
  // conservative pending reservation if that old callback is replayed.
  std::unordered_map<std::string, OrderState> orderStates_;
  std::unordered_set<std::string> unsupportedWorkingOrders_;
  bool orderLedgerUncertain_ = false;
  std::deque<std::string> terminalOrderKeys_;
  std::unordered_set<std::string> terminalOrderKeySet_;
  static constexpr size_t kMaxTerminalOrderTombstones = 65536;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      reservationKeysByRef_;
  std::unordered_map<std::string, std::string> reservationKeyBySys_;
  struct UnmatchedFill {
    std::string orderRef;
    double volume = 0.0;
  };
  // A trade from another terminal may precede its first OnRtnOrder. Remember
  // the already-applied quantity by exchange order id so the later Order row
  // cannot recreate it as pending exposure.
  std::unordered_map<std::string, UnmatchedFill> unmatchedFillsBySys_;
  static constexpr size_t kMaxUnmatchedFills = 65536;
  struct SeenTrade {
    std::string orderRef;
    std::string orderSysKey;
    std::string instrumentId;
    bool isBuy = false;
    bool isOpen = false;
    char hedgeFlag = '\0';
    double price = 0.0;
    double volume = 0.0;
  };
  std::unordered_map<std::string, SeenTrade> seenTrades_;
  std::map<std::string, std::vector<std::string>> tradeKeysByDay_;
  // Once a day's exact keys have been compacted, ignore callbacks from that
  // retired day. Otherwise one very late replay would be treated as new and,
  // worse, rotate a recent day out of the bounded exact-key window.
  // All trading days <= this YYYYMMDD cutoff have had their exact keys
  // compacted. A monotonic cutoff is bounded, unlike retaining one string per
  // historical day forever.
  std::string retiredThroughTradeDay_;
  static constexpr size_t kRetainedTradeDays = 3;
  // Exact dedupe must also be bounded inside one exceptionally busy/malformed
  // day. Older days are retired first. Exhausting one active day makes position
  // accounting fail closed until a later trading day plus authoritative sync.
  static constexpr size_t kMaxSeenTrades = 65536;
  std::string tradeDedupeExhaustedDay_;
  // A callback from a compacted day is ambiguous: it may be a duplicate or a
  // first-seen late fill. Never guess and under-count; opening inserts
  // fail closed until an authoritative position replacement clears this flag.
  bool ledgerUncertain_ = false;
  bool unsupportedRiskMutationUsed_ = false;
  std::atomic<uint64_t> revision_{0};
  static std::string resvKey(int frontId, int sessionId, const std::string &orderRef);
  void rememberTerminalOrderLocked(const std::string &key);
  void forgetTerminalOrderLocked(const std::string &key);
  void pruneEvictedTerminalOrderLocked(const std::string &key);
  // Contract multipliers are static metadata kept separate from position state
  // so held-position replacement/reset does not wipe them.
  std::unordered_map<std::string, double> multipliers_;
  std::unordered_map<std::string, bool> futuresMarginFormula_;
  // Per-instrument margin rates (fraction of notional), same lifetime as
  // multipliers_ (static metadata; survives resetPositions). Missing schedules
  // fail closed whenever a monetary cap needs them.
  struct MarginSchedule {
    double longByMoney = 0.0, longByVolume = 0.0;
    double shortByMoney = 0.0, shortByVolume = 0.0;
    bool isRelative = false;
  };
  std::unordered_map<std::string, MarginSchedule> marginRates_;
  std::unordered_map<std::string, MarginSchedule> exchangeMarginRates_;
  enum class MarginQueryKind : uint8_t { None, Investor, Exchange };
  int activeMarginQueryRequestId_ = 0;
  MarginQueryKind activeMarginQueryKind_ = MarginQueryKind::None;
  bool activeMarginQueryAuthorized_ = false;
  std::string activeMarginQueryInstrument_;
  char activeMarginQueryHedgeFlag_ = '\0';
  std::unordered_map<std::string, MarginSchedule> stagedMarginRates_;
  // Per-instrument, per-side lot caps (config metadata; like multipliers,
  // survives resets). 0 on a side = that side is uncapped.
  struct VolCap {
    double longMax = 0.0, shortMax = 0.0;
  };
  std::unordered_map<std::string, VolCap> maxPositionVol_;
  // Per-instrument scheduled-margin caps (config metadata; survives resets).
  std::unordered_map<std::string, double> maxInstrumentMargin_;
  // Best-effort lookup used only when no monetary cap is active. Capped paths
  // separately require multiplierKnownLocked(). Caller holds posMutex_.
  double multiplierLocked(const std::string &instrumentId) const;
  enum class AccountScope : uint8_t { Current, Foreign, Ambiguous };
  AccountScope accountScopeLocked(const std::string &brokerId,
                                  const std::string &investorId) const;
  void clearAccountMarginMetadataLocked();
  void clearAccountLedgerLocked();
  void setMarginScheduleLocked(const std::string &instrumentId,
                               char hedgeFlag, double longByMoney,
                               double longByVolume, double shortByMoney,
                               double shortByVolume, bool isRelative);
  void setExchangeMarginScheduleLocked(const std::string &instrumentId,
                                       char hedgeFlag, double longByMoney,
                                       double longByVolume,
                                       double shortByMoney,
                                       double shortByVolume);
  bool multiplierKnownLocked(const std::string &instrumentId) const;
  static std::string marginKey(const std::string &instrumentId, char hedgeFlag);
  bool marginCostLocked(const std::string &instrumentId, bool isLong,
                        char hedgeFlag, double price, double volume,
                        double &cost) const;
  bool hasCostCapLocked(const std::string &instrumentId) const;
  bool localControlsEnabledLocked() const;
  void refreshReservationCostsLocked(const std::string &instrumentId);
  void refreshAllReservationCostsLocked();
  void invalidateHeldCostsLocked(const std::string &instrumentId);
  void recomputeHeldSideLocked(Pos &position, bool isLong);
  void updateReservationExposureLocked(Reservation &r, double desiredVol,
                                       double price);
  void indexReservationLocked(const std::string &key);
  void unindexReservationLocked(const std::string &key,
                                const Reservation &r);
  // Returns true on an internal ownership conflict (ledger was made uncertain).
  bool observeInsertRequestLocked(const std::string &key,
                                  const Reservation &reservation);
  std::string reservationKeyForTradeLocked(
      const std::string &orderSysKey) const;
  void clearPendingLocked();
  static bool validOpenOrderSnapshot(
      const std::vector<OpenOrderInfo> &orders);
  bool rebuildOpenReservationsLocked(const std::vector<OpenOrderInfo> &orders);
  // Record + return a check() rejection (stores the reason for lastBlockReason).
  RiskVerdict block(const char *reason) const;
  // Per-calling-thread diagnostic: an armed-order rejection on the MD callback
  // thread cannot overwrite the reason a simultaneous JS-thread send is about
  // to read and surface.
  inline static thread_local const char *lastReason_ = nullptr;
};

} // namespace ctp

#endif /* CTP_NATIVE_RISK_H */
