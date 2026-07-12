/*
 * traderapi.cc - Trader N-API class (ObjectWrap).
 *
 * All requests funnel through _req(methodId, bytes, requestId) -> the generated
 * traderReq() switch, which builds the CTP struct from the JS-encoded bytes and
 * calls the matching ReqXxx. Pre-trade risk (kill-switch / max volume / max
 * notional / rate limit) is enforced in C++ on the order-insert path.
 *
 * Init() is deferred to _start() (after the JS drain is wired) so the
 * front-connected event can't be missed.
 */

#include "traderapi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../generated/layout.gen.h"
#include "../generated/traderreq.gen.h"
#include "../generated/traderspi.gen.h"
#include "ThostFtdcTraderApi.h"
#include "arm.h"
#include "channel.h"
#include "ctp_field.h"
#include "order_capability.h"
#include "risk.h"
#include "shared_handle.h"
#include "snapshot.h"

namespace ctp {

namespace {

// A per-instrument limit batch never legitimately spans more than the whole CTP
// instrument universe (a few tens of thousands). Reject anything larger up front
// so an attacker-controlled Array length cannot amplify into a huge reserve()
// whose std::bad_alloc would escape the Error-only N-API trampoline and abort
// the process instead of surfacing a catchable JS RangeError.
constexpr uint32_t kMaxInstrumentLimitBatch = 65536;

bool toStringList(const Napi::Value &v, std::vector<std::string> &out,
                  const char *label) {
  if (v.IsArray()) {
    Napi::Array a = v.As<Napi::Array>();
    for (uint32_t i = 0; i < a.Length(); ++i) {
      Napi::Value item = a.Get(i);
      if (!item.IsString()) {
        Napi::TypeError::New(v.Env(), std::string(label) +
                                         " must contain only strings")
            .ThrowAsJavaScriptException();
        return false;
      }
      std::string value = item.As<Napi::String>().Utf8Value();
      if (value.find('\0') != std::string::npos) {
        Napi::RangeError::New(v.Env(), std::string(label) +
                                          " must not contain NUL")
            .ThrowAsJavaScriptException();
        return false;
      }
      out.push_back(std::move(value));
    }
  } else if (v.IsString()) {
    std::string value = v.As<Napi::String>().Utf8Value();
    if (value.find('\0') != std::string::npos) {
      Napi::RangeError::New(v.Env(), std::string(label) +
                                        " must not contain NUL")
          .ThrowAsJavaScriptException();
      return false;
    }
    out.push_back(std::move(value));
  } else {
    Napi::TypeError::New(v.Env(), std::string(label) +
                                     " must be a string or string array")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

bool getFiniteNumber(const Napi::Object &object, const char *key,
                     double defaultValue, double &out) {
  if (!object.Has(key)) {
    out = defaultValue;
    return true;
  }
  Napi::Value value = object.Get(key);
  if (!value.IsNumber() ||
      !std::isfinite(value.As<Napi::Number>().DoubleValue())) {
    Napi::TypeError::New(object.Env(), std::string(key) +
                                           " must be a finite number")
        .ThrowAsJavaScriptException();
    return false;
  }
  out = value.As<Napi::Number>().DoubleValue();
  return true;
}

bool isInt32Number(const Napi::Value &v) {
  if (!v.IsNumber())
    return false;
  const double n = v.As<Napi::Number>().DoubleValue();
  return std::isfinite(n) && std::floor(n) == n && n >= -2147483648.0 &&
         n <= 2147483647.0;
}

bool validRiskLimitKey(const std::string &key) {
  static constexpr const char *keys[] = {
      "maxOrderVolume",       "maxPriceDeviation", "maxNotional",
      "maxOrdersPerSec",      "orderBurst",        "maxCloseOrdersPerSec",
      "closeOrderBurst",      "maxAccountMargin",  "maxReferenceAgeMs",
  };
  return std::any_of(std::begin(keys), std::end(keys),
                     [&](const char *candidate) { return key == candidate; });
}

bool testHooksEnabled(Napi::Env env) {
  Napi::Value process = env.Global().Get("process");
  if (!process.IsObject())
    return false;
  Napi::Value environment = process.As<Napi::Object>().Get("env");
  if (!environment.IsObject())
    return false;
  Napi::Value value =
      environment.As<Napi::Object>().Get("CTP_NODE_TEST_HOOKS");
  return value.IsString() && value.As<Napi::String>().Utf8Value() == "1";
}

} // namespace

class Trader : public Napi::ObjectWrap<Trader>, public OrderSink {
public:
  static Napi::Function Init(Napi::Env env);
  Trader(const Napi::CallbackInfo &info);
  ~Trader();

  // OrderSink: called on the MD callback thread when an armed trigger hits.
  int fireArmed(const ArmSpec &spec) override;

private:
  void doClose();
  bool detachBufferForClose();

  Napi::Value GetApiVersion(const Napi::CallbackInfo &info);
  Napi::Value GetTradingDay(const Napi::CallbackInfo &info);
  Napi::Value Req(const Napi::CallbackInfo &info);
  Napi::Value SetRiskLimits(const Napi::CallbackInfo &info);
  Napi::Value RiskReduceOnly(const Napi::CallbackInfo &info);
  Napi::Value RiskHaltAll(const Napi::CallbackInfo &info);
  Napi::Value RiskResume(const Napi::CallbackInfo &info);
  Napi::Value LastRiskReason(const Napi::CallbackInfo &info);
  Napi::Value TimeoutSessionTransition(const Napi::CallbackInfo &info);
  Napi::Value ConfirmSessionAccount(const Napi::CallbackInfo &info);
  Napi::Value BeginSessionHandshake(const Napi::CallbackInfo &info);
  Napi::Value AttachSnapshot(const Napi::CallbackInfo &info);
  Napi::Value SetInstrumentPositionLimits(const Napi::CallbackInfo &info);
  Napi::Value SetInstrumentMarginLimits(const Napi::CallbackInfo &info);
  Napi::Value RebuildReservations(const Napi::CallbackInfo &info);
  Napi::Value RiskRevision(const Napi::CallbackInfo &info);
  Napi::Value MarkPositionLedgerUncertain(const Napi::CallbackInfo &info);
  Napi::Value MarkOrderLedgerUncertain(const Napi::CallbackInfo &info);
  Napi::Value ReplacePositions(const Napi::CallbackInfo &info);
  Napi::Value AccountMargin(const Napi::CallbackInfo &info);
  Napi::Value ResetPositions(const Napi::CallbackInfo &info);
  Napi::Value Start(const Napi::CallbackInfo &info);
  Napi::Value Buffer(const Napi::CallbackInfo &info);
  Napi::Value ClaimBatch(const Napi::CallbackInfo &info);
  Napi::Value ReleaseBatch(const Napi::CallbackInfo &info);
  Napi::Value DropCount(const Napi::CallbackInfo &info);
  Napi::Value ChannelInfo(const Napi::CallbackInfo &info);
  Napi::Value Close(const Napi::CallbackInfo &info);
  Napi::Value ArmRegistryExternal(const Napi::CallbackInfo &info);
  Napi::Value Arm(const Napi::CallbackInfo &info);
  Napi::Value Disarm(const Napi::CallbackInfo &info);
  Napi::Value ArmFireCount(const Napi::CallbackInfo &info);
  Napi::Value ArmBlockedCount(const Napi::CallbackInfo &info);
  Napi::Value BurstEvents(const Napi::CallbackInfo &info);
  Napi::Value InjectTestLogin(const Napi::CallbackInfo &info);
  int nextArmRequestId();

  CThostFtdcTraderApi *api_ = nullptr;
  TraderSpi *spi_ = nullptr;
  EventChannel *ch_ = nullptr;
  RiskEngine risk_;
  std::shared_ptr<ArmRegistry> arm_ = std::make_shared<ArmRegistry>();
  // Disjoint from JS CtpClient ids (1..0x3fffffff), preventing an armed-order
  // error response from settling an unrelated pending query by numeric id.
  std::atomic<uint32_t> armReqId_{0x40000000u};
  std::thread burst_; // test-only lossless-channel stress producer
  napi_env env_ = nullptr;
  napi_ref bufferRef_ = nullptr;
  bool started_ = false;
  bool closed_ = false;
};

Napi::Function Trader::Init(Napi::Env env) {
  return DefineClass(
      env, "Trader",
      {
          InstanceMethod("apiVersion", &Trader::GetApiVersion),
          InstanceMethod("tradingDay", &Trader::GetTradingDay),
          InstanceMethod("_req", &Trader::Req),
          InstanceMethod("setRiskLimits", &Trader::SetRiskLimits),
          InstanceMethod("riskReduceOnly", &Trader::RiskReduceOnly),
          InstanceMethod("riskHaltAll", &Trader::RiskHaltAll),
          InstanceMethod("riskResume", &Trader::RiskResume),
          InstanceMethod("lastRiskReason", &Trader::LastRiskReason),
          InstanceMethod("_timeoutSessionTransition",
                         &Trader::TimeoutSessionTransition),
          InstanceMethod("_confirmSessionAccount",
                         &Trader::ConfirmSessionAccount),
          InstanceMethod("_beginSessionHandshake",
                         &Trader::BeginSessionHandshake),
          InstanceMethod("_injectTestLogin", &Trader::InjectTestLogin),
          InstanceMethod("_attachSnapshot", &Trader::AttachSnapshot),
          InstanceMethod("_setInstrumentPositionLimits",
                         &Trader::SetInstrumentPositionLimits),
          InstanceMethod("_setInstrumentMarginLimits",
                         &Trader::SetInstrumentMarginLimits),
          InstanceMethod("rebuildReservations", &Trader::RebuildReservations),
          InstanceMethod("riskRevision", &Trader::RiskRevision),
          InstanceMethod("_markPositionLedgerUncertain",
                         &Trader::MarkPositionLedgerUncertain),
          InstanceMethod("_markOrderLedgerUncertain",
                         &Trader::MarkOrderLedgerUncertain),
          InstanceMethod("replacePositions", &Trader::ReplacePositions),
          InstanceMethod("accountMargin", &Trader::AccountMargin),
          InstanceMethod("resetPositions", &Trader::ResetPositions),
          InstanceMethod("_start", &Trader::Start),
          InstanceMethod("_buffer", &Trader::Buffer),
          InstanceMethod("_claim", &Trader::ClaimBatch),
          InstanceMethod("_release", &Trader::ReleaseBatch),
          InstanceMethod("_dropCount", &Trader::DropCount),
          InstanceMethod("_info", &Trader::ChannelInfo),
          InstanceMethod("_armRegistry", &Trader::ArmRegistryExternal),
          InstanceMethod("arm", &Trader::Arm),
          InstanceMethod("disarm", &Trader::Disarm),
          InstanceMethod("_armFireCount", &Trader::ArmFireCount),
          InstanceMethod("_armBlockedCount", &Trader::ArmBlockedCount),
          InstanceMethod("_burstEvents", &Trader::BurstEvents),
          InstanceMethod("close", &Trader::Close),
      });
}

Trader::Trader(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<Trader>(info), env_(info.Env()) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Trader(flowPath, fronts)")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string flowPath = info[0].As<Napi::String>().Utf8Value();
  if (flowPath.find('\0') != std::string::npos) {
    Napi::RangeError::New(env, "Trader flowPath must not contain NUL")
        .ThrowAsJavaScriptException();
    return;
  }

  // Validate fronts BEFORE allocating anything: RegisterFront("") faults deep
  // inside the CTP DLL, so drop empty addresses and reject an empty result with
  // a clean JS error rather than crashing the process.
  std::vector<std::string> fronts;
  std::vector<std::string> requestedFronts;
  if (!toStringList(info[1], requestedFronts, "fronts"))
    return;
  for (auto &addr : requestedFronts)
    if (!addr.empty())
      fronts.push_back(addr);
  if (fronts.empty()) {
    Napi::TypeError::New(
        env, "Trader: at least one non-empty front address is required")
        .ThrowAsJavaScriptException();
    return;
  }

  api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flowPath.c_str());
  if (!api_) {
    Napi::Error::New(env, "CreateFtdcTraderApi failed")
        .ThrowAsJavaScriptException();
    return;
  }
  // Trader uses a lossless staged overflow: order/trade/query returns must never
  // be silently discarded under backpressure.
  ch_ = new EventChannel(4096, static_cast<size_t>(ctpMaxStructSize()),
                          DropPolicy::Lossless);
  spi_ = new TraderSpi(ch_);
  spi_->setRisk(&risk_); // fills update the position-margin tracker
  spi_->setArmRegistry(arm_.get()); // front transitions revoke old mandates
  // Fail closed even before the first OnFrontConnected callback. Opening
  // permission must come only from authoritative position + working-order
  // snapshots, never from a timing window during construction/initial login.
  risk_.markTradeLedgerUncertain();
  risk_.markOrderLedgerUncertain();
  api_->RegisterSpi(spi_);
  arm_->setSink(this); // armed triggers fire orders through this Trader
  api_->SubscribePublicTopic(THOST_TERT_QUICK);
  api_->SubscribePrivateTopic(THOST_TERT_QUICK);

  for (auto &addr : fronts)
    api_->RegisterFront(const_cast<char *>(addr.c_str()));
}

Trader::~Trader() { doClose(); }

bool Trader::detachBufferForClose() {
  if (!bufferRef_)
    return true;

  // Fail closed on every N-API anomaly: an attached external ArrayBuffer is a
  // reason to retain native backing storage, never a reason to risk a UAF.
  bool safeToFreeRing = false;
  napi_value buffer = nullptr;
  if (env_ &&
      napi_get_reference_value(env_, bufferRef_, &buffer) == napi_ok &&
      buffer != nullptr) {
    bool detached = false;
    if (napi_is_detached_arraybuffer(env_, buffer, &detached) == napi_ok) {
      if (detached) {
        safeToFreeRing = true;
      } else if (napi_detach_arraybuffer(env_, buffer) == napi_ok) {
        detached = false;
        safeToFreeRing =
            napi_is_detached_arraybuffer(env_, buffer, &detached) == napi_ok &&
            detached;
      }
    }
  }
  if (env_)
    napi_delete_reference(env_, bufferRef_);
  bufferRef_ = nullptr;
  return safeToFreeRing;
}

void Trader::doClose() {
  if (closed_)
    return;
  closed_ = true;
  if (burst_.joinable())
    burst_.join();
  // Order matters: clearSink() (under the arm lock) guarantees no fireArmed is
  // running or will start, so the non-atomic api_ below is never read after it
  // is nulled. Do NOT move the api_ teardown before clearSink().
  arm_->clearSink();
  if (api_) {
    api_->Release();
    api_ = nullptr;
  }
  // The ring backs one cached external ArrayBuffer. Detach it before freeing
  // the native storage so retained JS references can never read freed memory.
  const bool safeToFreeRing = detachBufferForClose();
  // Free the SPI and the ring here, on close(), not in the GC finalizer: the
  // ring (~maxStructSize * 4096 slots, ~10 MB) is off-heap so V8 feels no GC
  // pressure from it, and a create/close loop leaks it (measured unreclaimed
  // even by a forced GC) until finalizers eventually run. Release() above
  // stopped the CTP callback thread, so no more pushes; accessors null-check.
  if (spi_) {
    delete spi_;
    spi_ = nullptr;
  }
  if (ch_) {
    ch_->stop();
    if (safeToFreeRing)
      delete ch_;
    // A stopped-channel leak is preferable to exposing freed storage through
    // an ArrayBuffer if detachment unexpectedly failed.
    ch_ = nullptr;
  }
}

int Trader::fireArmed(const ArmSpec &spec) {
  CThostFtdcTraderApi *api = api_;
  if (!api)
    return -1;
  CThostFtdcInputOrderField f;
  std::memset(&f, 0, sizeof(f));
  std::memcpy(&f, spec.orderTemplate.data(),
              std::min(spec.orderTemplate.size(), sizeof(f)));
  OrdinaryOrderIdentity identity;
  if (!decodeOrdinaryOrder(f, identity))
    return CTP_INVALID_ORDER_FIELDS;
  const bool isOpen = f.CombOffsetFlag[0] == '0';
  bool sendAllowed = false;
  auto sendGuard = risk_.acquireOrderSendForAccount(
      isOpen, identity.brokerId, identity.investorId, sendAllowed);
  if (!sendAllowed)
    return CTP_RISK_BLOCKED;
  // Armed orders use the same fail-closed gate as regular orders. In particular,
  // a configured deviation check may not silently skip stale/missing market data.
  RiskVerdict v = risk_.check(identity.instrumentId, f.LimitPrice,
                              risk_.refPrice(identity.instrumentId),
                              f.VolumeTotalOriginal, isOpen);
  if (!v.ok)
    return CTP_RISK_BLOCKED;
  int requestId = 0;
  // Position caps (incl. in-flight reservation) apply to armed orders too -
  // safety, not latency-sensitive.
  if (isOpen) {
    OpenGate g = risk_.tryReserveOpen(identity.orderRef, identity.instrumentId,
                                      f.Direction == '0', f.LimitPrice,
                                      f.VolumeTotalOriginal,
                                      f.CombHedgeFlag[0]);
    if (g == OpenGate::VolumeLimit)
      return CTP_POSITION_VOLUME_LIMIT;
    if (g == OpenGate::CostLimit)
      return CTP_POSITION_LIMIT;
    if (g == OpenGate::DuplicateOrderRef)
      return CTP_DUPLICATE_ORDER_REF;
    if (g == OpenGate::MetadataUnavailable)
      return CTP_RISK_METADATA_UNAVAILABLE;
    requestId = nextArmRequestId();
    if (!risk_.bindInsertRequest(requestId, identity.orderRef))
      return CTP_RISK_METADATA_UNAVAILABLE;
  }
  if (isOpen && !risk_.allowRate()) {
    risk_.rollbackInsertRequest(requestId, identity.orderRef);
    return CTP_RATE_LIMITED;
  }
  if (!isOpen && !risk_.allowCloseRate())
    return CTP_RATE_LIMITED;
  if (requestId == 0)
    requestId = nextArmRequestId();
  int sendRc = api->ReqOrderInsert(&f, requestId);
  if (sendRc != 0 && isOpen)
    risk_.rollbackInsertRequest(requestId, identity.orderRef); // not accepted; arm may retry this ref
  return sendRc;
}

int Trader::nextArmRequestId() {
  uint32_t current = armReqId_.load(std::memory_order_relaxed);
  for (;;) {
    const uint32_t next = current >= 0x7fffffffu ? 0x40000000u : current + 1u;
    if (armReqId_.compare_exchange_weak(current, next,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed))
      return static_cast<int>(current);
  }
}

Napi::Value Trader::ArmRegistryExternal(const Napi::CallbackInfo &info) {
  if (closed_ || !api_ || !arm_->hasSink()) {
    Napi::Error::New(info.Env(), "Trader is closed")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  auto *handle = new SharedHandle();
  handle->kind = SharedHandleKind::ArmRegistry;
  handle->arm = arm_;
  return Napi::External<SharedHandle>::New(
      info.Env(), handle, [](Napi::Env, SharedHandle *p) { delete p; });
}

Napi::Value Trader::Arm(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (closed_ || !api_ || !arm_->hasSink()) {
    Napi::Error::New(env, "Trader is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString() ||
      !info[2].IsNumber() || !info[3].IsTypedArray()) {
    Napi::TypeError::New(env, "invalid armed-order arguments")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::TypedArray typed = info[3].As<Napi::TypedArray>();
  if (typed.TypedArrayType() != napi_uint8_array) {
    Napi::TypeError::New(env, "armed order template must be a Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Uint8Array bytes = info[3].As<Napi::Uint8Array>();
  if (bytes.ByteLength() != sizeof(CThostFtdcInputOrderField)) {
    Napi::RangeError::New(env, "invalid armed InputOrder byte length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CThostFtdcInputOrderField order;
  std::memcpy(&order, bytes.Data(), sizeof(order));
  OrdinaryOrderIdentity identity;
  if (!decodeOrdinaryOrder(order, identity)) {
    Napi::TypeError::New(env, "unsupported armed order template")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ArmSpec spec;
  spec.instrumentId = info[0].As<Napi::String>().Utf8Value();
  std::string side = info[1].As<Napi::String>().Utf8Value();
  spec.side = side.size() == 1 ? side[0] : '\0';
  spec.triggerPrice = info[2].As<Napi::Number>().DoubleValue();
  if (spec.instrumentId != identity.instrumentId ||
      (spec.side != '0' && spec.side != '1') || spec.side != order.Direction ||
      !std::isfinite(spec.triggerPrice) || spec.triggerPrice <= 0.0 ||
      spec.triggerPrice >= 1e300) {
    Napi::TypeError::New(env, "armed trigger does not match its order template")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  spec.orderTemplate.assign(bytes.Data(), bytes.Data() + bytes.ByteLength());
  const bool isOpen = order.CombOffsetFlag[0] == '0';
  bool allowed = false;
  // Gate registration on the risk MODE only: a HaltAll (and, for opens,
  // ReduceOnly) hard-stop must not accept new arms. The account/session scope is
  // deliberately NOT checked here — at the native layer an arm may legitimately
  // be registered before login and fire once the session is confirmed. (The
  // TypeScript arm() wrapper is stricter still: it needs a confirmed login to
  // stamp the order template's broker/investor.) fireArmed() re-applies the full
  // per-account gate at send time, so no armed order can reach CTP in an
  // unconfirmed or foreign account regardless of when it was registered.
  auto sendGuard = risk_.acquireOrderSend(isOpen, allowed);
  if (!allowed) {
    Napi::Error::New(env, "cannot arm while order sending is halted")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint64_t id = arm_->arm(std::move(spec));
  return Napi::Number::New(env, static_cast<double>(id));
}

Napi::Value Trader::Disarm(const Napi::CallbackInfo &info) {
  uint64_t id = static_cast<uint64_t>(info[0].As<Napi::Number>().DoubleValue());
  return Napi::Boolean::New(info.Env(), arm_->disarm(id));
}

Napi::Value Trader::ArmBlockedCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(),
                           static_cast<double>(arm_->blockedCount()));
}

Napi::Value Trader::ArmFireCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(),
                           static_cast<double>(arm_->fireCount()));
}

Napi::Value Trader::BurstEvents(const Napi::CallbackInfo &info) {
  if (!testHooksEnabled(info.Env())) {
    Napi::Error::New(info.Env(),
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  if (!ch_ || closed_)
    return info.Env().Undefined();
  const int n = info[0].As<Napi::Number>().Int32Value();
  if (n <= 0)
    return info.Env().Undefined();
  if (burst_.joinable())
    burst_.join();
  EventChannel *ch = ch_;
  burst_ = std::thread([ch, n]() {
    for (int i = 0; i < n; ++i)
      ch->push(ET_RtnTrade, i, -1, 0, "", -2, &i, sizeof(i));
  });
  return info.Env().Undefined();
}

Napi::Value Trader::GetApiVersion(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), CThostFtdcTraderApi::GetApiVersion());
}

Napi::Value Trader::GetTradingDay(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::String::New(env, "");
  const char *d = api_->GetTradingDay();
  return Napi::String::New(env, d ? d : "");
}

Napi::Value Trader::Req(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::Number::New(env, -1);
  int methodId = info[0].As<Napi::Number>().Int32Value();
  if (methodId == M_ReqUserLogin || methodId == M_ReqUserLogout ||
      methodId == M_ReqUserLoginWithCaptcha ||
      methodId == M_ReqUserLoginWithText ||
      methodId == M_ReqUserLoginWithOTP) {
    // A mandate belongs to one authenticated session/account. Clear it before
    // the transition request; traderReq() then invalidates the risk identity
    // before calling CTP. A failed transition stays deliberately disarmed.
    arm_->clear();
  }
  Napi::Uint8Array bytes = info[1].As<Napi::Uint8Array>();
  int id = info[2].As<Napi::Number>().Int32Value();
  int rc = traderReq(api_, methodId, bytes.Data(),
                     static_cast<int>(bytes.ByteLength()), id, &risk_);
  return Napi::Number::New(env, rc);
}

Napi::Value Trader::SetRiskLimits(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject() || info[0].IsArray()) {
    Napi::TypeError::New(env, "setRiskLimits requires an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object object = info[0].As<Napi::Object>();
  Napi::Array names = object.GetPropertyNames();
  for (uint32_t i = 0; i < names.Length(); ++i) {
    Napi::Value name = names.Get(i);
    if (!name.IsString() ||
        !validRiskLimitKey(name.As<Napi::String>().Utf8Value())) {
      Napi::TypeError::New(env, "setRiskLimits contains an unknown field")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
  }
  RiskLimits cfg;
  double maxOrderVolume = 0.0;
  if (!getFiniteNumber(object, "maxOrderVolume", 0.0, maxOrderVolume) ||
      !getFiniteNumber(object, "maxPriceDeviation", 0.0,
                       cfg.maxPriceDeviation) ||
      !getFiniteNumber(object, "maxNotional", 0.0, cfg.maxNotional) ||
      !getFiniteNumber(object, "maxOrdersPerSec", 0.0,
                       cfg.maxOrdersPerSec) ||
      !getFiniteNumber(object, "orderBurst", 0.0, cfg.orderBurst) ||
      !getFiniteNumber(object, "maxCloseOrdersPerSec", 0.0,
                       cfg.maxCloseOrdersPerSec) ||
      !getFiniteNumber(object, "closeOrderBurst", 0.0,
                       cfg.closeOrderBurst) ||
      !getFiniteNumber(object, "maxAccountMargin", 0.0,
                       cfg.maxAccountMargin) ||
      !getFiniteNumber(object, "maxReferenceAgeMs", 3000.0,
                       cfg.maxReferenceAgeMs))
    return env.Undefined();
  if (maxOrderVolume < 0.0 || cfg.maxPriceDeviation < 0.0 ||
      cfg.maxNotional < 0.0 || cfg.maxOrdersPerSec < 0.0 ||
      cfg.orderBurst < 0.0 || cfg.maxCloseOrdersPerSec < 0.0 ||
      cfg.closeOrderBurst < 0.0 || cfg.maxAccountMargin < 0.0 ||
      (maxOrderVolume > 0.0 &&
       (std::floor(maxOrderVolume) != maxOrderVolume ||
        maxOrderVolume > 2147483647.0)) ||
      cfg.maxReferenceAgeMs <= 0.0) {
    Napi::TypeError::New(env, "invalid risk-limit value")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  cfg.maxOrderVolume = maxOrderVolume > 0.0
                           ? static_cast<int>(maxOrderVolume)
                           : 0;
  risk_.configure(cfg);
  return env.Undefined();
}

Napi::Value Trader::RiskReduceOnly(const Napi::CallbackInfo &info) {
  risk_.reduceOnly();
  return info.Env().Undefined();
}

Napi::Value Trader::RiskHaltAll(const Napi::CallbackInfo &info) {
  // First close the final send barrier (and wait for any already-admitted send
  // to return), then discard historical mandates. After this method returns,
  // resume() cannot unexpectedly fire an arm created before the hard stop.
  risk_.haltAll();
  arm_->clear();
  return info.Env().Undefined();
}

Napi::Value Trader::RiskResume(const Napi::CallbackInfo &info) {
  risk_.resume();
  return info.Env().Undefined();
}

// Reason string of the most recent pre-trade-risk (check) rejection, "" if none.
Napi::Value Trader::LastRiskReason(const Napi::CallbackInfo &info) {
  const char *r = risk_.lastBlockReason();
  return Napi::String::New(info.Env(), r ? r : "");
}

Napi::Value Trader::TimeoutSessionTransition(
    const Napi::CallbackInfo &info) {
  if (info.Length() < 1 || !info[0].IsNumber())
    return Napi::Boolean::New(info.Env(), true);
  const double raw = info[0].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(raw) || std::floor(raw) != raw || raw <= 0.0 ||
      raw > 2147483647.0)
    return Napi::Boolean::New(info.Env(), true);
  return Napi::Boolean::New(
      info.Env(),
      risk_.timeoutSessionTransition(static_cast<int>(raw)));
}

Napi::Value Trader::ConfirmSessionAccount(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !isInt32Number(info[0]) ||
      !isInt32Number(info[1]) || !info[2].IsTypedArray()) {
    Napi::TypeError::New(
        env, "_confirmSessionAccount requires frontId, sessionId and encoded SettlementInfoConfirm bytes")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::TypedArray typed = info[2].As<Napi::TypedArray>();
  if (typed.TypedArrayType() != napi_uint8_array) {
    Napi::TypeError::New(
        env, "_confirmSessionAccount bytes must be a Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Uint8Array bytes = info[2].As<Napi::Uint8Array>();
  if (bytes.ByteLength() != sizeof(CThostFtdcSettlementInfoConfirmField)) {
    Napi::RangeError::New(
        env, "invalid encoded SettlementInfoConfirm byte length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CThostFtdcSettlementInfoConfirmField account;
  std::memcpy(&account, bytes.Data(), sizeof(account));
  std::string brokerId, investorId;
  if (!boundedCtpField(account.BrokerID, brokerId) ||
      !boundedCtpField(account.InvestorID, investorId) || brokerId.empty() ||
      investorId.empty()) {
    Napi::RangeError::New(env, "invalid encoded session account identity")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int frontId = info[0].As<Napi::Number>().Int32Value();
  const int sessionId = info[1].As<Napi::Number>().Int32Value();
  return Napi::Boolean::New(
      env, risk_.confirmSessionAccount(frontId, sessionId, brokerId,
                                       investorId));
}

Napi::Value Trader::BeginSessionHandshake(const Napi::CallbackInfo &info) {
  if (arm_)
    arm_->clear();
  risk_.invalidateSession();
  return info.Env().Undefined();
}

// Test-only: establish a confirmed native session offline. Drives the exact
// same RiskEngine handshake the live path uses (beginSessionTransition ->
// acceptLoginSession, as the SPI OnRspUserLogin would, then confirmSessionAccount
// as the high-level session() does), so an order/arm test can reach a confirmed
// account without a live front. Gated behind CTP_NODE_TEST_HOOKS like the other
// injection hooks; returns whether the confirmed session was established.
Napi::Value Trader::InjectTestLogin(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!testHooksEnabled(env)) {
    Napi::Error::New(env,
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 4 || !isInt32Number(info[0]) || !isInt32Number(info[1]) ||
      !info[2].IsString() || !info[3].IsString()) {
    Napi::TypeError::New(
        env, "_injectTestLogin requires frontId, sessionId, brokerId, investorId")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int frontId = info[0].As<Napi::Number>().Int32Value();
  const int sessionId = info[1].As<Napi::Number>().Int32Value();
  const std::string brokerId = info[2].As<Napi::String>().Utf8Value();
  const std::string investorId = info[3].As<Napi::String>().Utf8Value();
  // Drives pendingSessionRequestId_ (the session-transition slot) — a separate
  // structure from the insert/arm request bindings (insertRequests_), so this id
  // need not be unique across those spaces (nextArmRequestId can also return this
  // value). acceptLoginSession clears it immediately, leaving no pending state.
  static constexpr int kTestLoginRequestId = 2147483647;
  risk_.beginSessionTransition(kTestLoginRequestId, true, brokerId, investorId);
  if (!risk_.acceptLoginSession(kTestLoginRequestId, frontId, sessionId,
                                brokerId, investorId))
    return Napi::Boolean::New(env, false);
  return Napi::Boolean::New(
      env, risk_.confirmSessionAccount(frontId, sessionId, brokerId, investorId));
}

// (mdSnapshotExternal) - attach a MarketData's snapshot cache so the deviation
// check reads live prices in C++ (no JS round-trip). See Trader.trackMarketData.
Napi::Value Trader::AttachSnapshot(const Napi::CallbackInfo &info) {
  if (info.Length() < 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(info.Env(), "invalid MarketData snapshot handle")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  auto ext = info[0].As<Napi::External<SharedHandle>>();
  SharedHandle *handle = ext.Data();
  if (!handle || handle->magic != SharedHandle::kMagic ||
      handle->kind != SharedHandleKind::SnapshotCache || !handle->snapshot) {
    Napi::TypeError::New(info.Env(), "invalid MarketData snapshot handle")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  risk_.setMdSnapshot(handle->snapshot);
  return info.Env().Undefined();
}

// (Array<{instrumentId, long?, short?}>) - one atomic position-limit batch.
Napi::Value Trader::SetInstrumentPositionLimits(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "position limits must be an array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  if (input.Length() > kMaxInstrumentLimitBatch) {
    Napi::RangeError::New(
        env, "position limits batch exceeds the maximum of 65536 instruments")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<InstrumentPositionLimitUpdate> updates;
  updates.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value rowValue = input.Get(i);
    if (!rowValue.IsObject()) {
      Napi::TypeError::New(env, "each position limit must be an object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object row = rowValue.As<Napi::Object>();
    Napi::Value instrument = row.Get("instrumentId");
    Napi::Value longValue = row.Get("long");
    Napi::Value shortValue = row.Get("short");
    if (!instrument.IsString() ||
        (!longValue.IsUndefined() && !longValue.IsNumber()) ||
        (!shortValue.IsUndefined() && !shortValue.IsNumber())) {
      Napi::TypeError::New(env, "invalid position-limit entry")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    InstrumentPositionLimitUpdate update;
    update.instrumentId = instrument.As<Napi::String>().Utf8Value();
    update.updateLong = longValue.IsNumber();
    update.updateShort = shortValue.IsNumber();
    if (update.updateLong)
      update.longLimit = longValue.As<Napi::Number>().DoubleValue();
    if (update.updateShort)
      update.shortLimit = shortValue.As<Napi::Number>().DoubleValue();
    updates.push_back(std::move(update));
  }
  if (!risk_.setInstrumentPositionLimits(updates)) {
    Napi::TypeError::New(env, "invalid position-limit batch")
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

// (Array<{instrumentId, limit}>) - one atomic scheduled-margin-limit batch.
Napi::Value Trader::SetInstrumentMarginLimits(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "margin limits must be an array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  if (input.Length() > kMaxInstrumentLimitBatch) {
    Napi::RangeError::New(
        env, "margin limits batch exceeds the maximum of 65536 instruments")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<InstrumentMarginLimitUpdate> updates;
  updates.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value rowValue = input.Get(i);
    if (!rowValue.IsObject()) {
      Napi::TypeError::New(env, "each margin limit must be an object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object row = rowValue.As<Napi::Object>();
    Napi::Value instrument = row.Get("instrumentId");
    Napi::Value limit = row.Get("limit");
    if (!instrument.IsString() || !limit.IsNumber()) {
      Napi::TypeError::New(env, "invalid margin-limit entry")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    updates.push_back(InstrumentMarginLimitUpdate{
        instrument.As<Napi::String>().Utf8Value(),
        limit.As<Napi::Number>().DoubleValue()});
  }
  if (!risk_.setInstrumentMarginLimits(updates)) {
    Napi::TypeError::New(env, "invalid margin-limit batch")
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

// (Array<{frontId, sessionId, orderRef, instrumentId, isLong, vol, price}>) -
// rebuild in-flight reservations from CTP's working orders (reqQryOrder).
Napi::Value Trader::RebuildReservations(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "rebuildReservations requires rows and revision")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }
  std::vector<OpenOrderInfo> orders;
  Napi::Array arr = info[0].As<Napi::Array>();
  for (uint32_t i = 0; i < arr.Length(); ++i) {
    Napi::Value v = arr.Get(i);
    if (!v.IsObject()) {
      Napi::TypeError::New(env, "invalid working-order snapshot row")
          .ThrowAsJavaScriptException();
      return Napi::Boolean::New(env, false);
    }
    Napi::Object o = v.As<Napi::Object>();
    Napi::Value front = o.Get("frontId");
    Napi::Value session = o.Get("sessionId");
    Napi::Value orderRef = o.Get("orderRef");
    Napi::Value instrument = o.Get("instrumentId");
    Napi::Value isLong = o.Get("isLong");
    Napi::Value volume = o.Get("vol");
    Napi::Value price = o.Get("price");
    Napi::Value hedgeValue = o.Get("hedgeFlag");
    Napi::Value tradedValue = o.Get("traded");
    Napi::Value sysValue = o.Get("orderSysKey");
    Napi::Value supportedValue = o.Get("supportedSingleLeg");
    if (!isInt32Number(front) || !isInt32Number(session) ||
        !orderRef.IsString() ||
        !instrument.IsString() || !isLong.IsBoolean() || !volume.IsNumber() ||
        !price.IsNumber() ||
        !hedgeValue.IsString() ||
        (!tradedValue.IsUndefined() && !tradedValue.IsNumber()) ||
        (!sysValue.IsUndefined() && !sysValue.IsString()) ||
        !supportedValue.IsBoolean()) {
      Napi::TypeError::New(env, "invalid working-order snapshot row")
          .ThrowAsJavaScriptException();
      return Napi::Boolean::New(env, false);
    }
    OpenOrderInfo r;
    r.frontId = front.As<Napi::Number>().Int32Value();
    r.sessionId = session.As<Napi::Number>().Int32Value();
    r.orderRef = orderRef.As<Napi::String>().Utf8Value();
    r.instrumentId = instrument.As<Napi::String>().Utf8Value();
    r.isLong = isLong.As<Napi::Boolean>().Value();
    const std::string hedge = hedgeValue.As<Napi::String>().Utf8Value();
    r.hedgeFlag = hedge.empty() ? '\0' : hedge[0];
    r.vol = volume.As<Napi::Number>().DoubleValue();
    r.price = price.As<Napi::Number>().DoubleValue();
    r.traded = tradedValue.IsNumber()
                   ? tradedValue.As<Napi::Number>().DoubleValue()
                   : 0.0;
    r.orderSysKey = sysValue.IsString()
                        ? sysValue.As<Napi::String>().Utf8Value()
                        : std::string();
    r.supportedSingleLeg = supportedValue.As<Napi::Boolean>().Value();
    orders.push_back(std::move(r));
  }
  const double revision = info[1].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(revision) || revision < 0.0 ||
      revision > 9007199254740991.0 || std::floor(revision) != revision) {
    Napi::TypeError::New(env, "invalid risk revision")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }
  return Napi::Boolean::New(
      env, risk_.rebuildOpenReservationsIfRevision(
               static_cast<uint64_t>(revision), orders));
}

Napi::Value Trader::RiskRevision(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), static_cast<double>(risk_.revision()));
}

Napi::Value Trader::MarkPositionLedgerUncertain(
    const Napi::CallbackInfo &info) {
  risk_.markTradeLedgerUncertain();
  return info.Env().Undefined();
}

Napi::Value Trader::MarkOrderLedgerUncertain(
    const Napi::CallbackInfo &info) {
  risk_.markOrderLedgerUncertain();
  return info.Env().Undefined();
}

Napi::Value Trader::ReplacePositions(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "replacePositions requires rows and revision")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }
  std::vector<PositionSeed> seeds;
  Napi::Array arr = info[0].As<Napi::Array>();
  for (uint32_t i = 0; i < arr.Length(); ++i) {
    Napi::Value v = arr.Get(i);
    if (!v.IsObject()) {
      Napi::TypeError::New(env, "invalid position snapshot row")
          .ThrowAsJavaScriptException();
      return Napi::Boolean::New(env, false);
    }
    Napi::Object o = v.As<Napi::Object>();
    Napi::Value instrument = o.Get("instrumentId");
    Napi::Value isLong = o.Get("isLong");
    Napi::Value hedge = o.Get("hedgeFlag");
    Napi::Value volume = o.Get("volume");
    Napi::Value margin = o.Get("margin");
    if (!instrument.IsString() || !isLong.IsBoolean() || !hedge.IsString() ||
        !volume.IsNumber() || !margin.IsNumber()) {
      Napi::TypeError::New(env, "invalid position snapshot row")
          .ThrowAsJavaScriptException();
      return Napi::Boolean::New(env, false);
    }
    const std::string hedgeFlag = hedge.As<Napi::String>().Utf8Value();
    if (hedgeFlag.size() != 1) {
      Napi::TypeError::New(env, "invalid position snapshot hedgeFlag")
          .ThrowAsJavaScriptException();
      return Napi::Boolean::New(env, false);
    }
    seeds.push_back(PositionSeed{
        instrument.As<Napi::String>().Utf8Value(),
        isLong.As<Napi::Boolean>().Value(),
        volume.As<Napi::Number>().DoubleValue(),
        margin.As<Napi::Number>().DoubleValue(), hedgeFlag[0]});
  }
  const double revision = info[1].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(revision) || revision < 0.0 ||
      revision > 9007199254740991.0 || std::floor(revision) != revision) {
    Napi::TypeError::New(env, "invalid risk revision")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }
  return Napi::Boolean::New(
      env, risk_.replacePositionsIfRevision(static_cast<uint64_t>(revision),
                                            seeds));
}

Napi::Value Trader::AccountMargin(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), risk_.accountMargin());
}

Napi::Value Trader::ResetPositions(const Napi::CallbackInfo &info) {
  risk_.resetPositions();
  return info.Env().Undefined();
}

Napi::Value Trader::Start(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (started_ || !api_)
    return env.Undefined();
  napi_status st = ch_->start(env, info[0]);
  if (st != napi_ok) {
    Napi::Error::New(env, "failed to start event channel")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  api_->Init();
  started_ = true;
  return env.Undefined();
}

Napi::Value Trader::Buffer(const Napi::CallbackInfo &info) {
  if (!ch_)
    return info.Env().Undefined();
  if (bufferRef_) {
    napi_value cached = nullptr;
    if (napi_get_reference_value(info.Env(), bufferRef_, &cached) == napi_ok &&
        cached != nullptr)
      return Napi::Value(info.Env(), cached);
    Napi::Error::New(info.Env(), "cached event-channel buffer is unavailable")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  Napi::ArrayBuffer buffer =
      Napi::ArrayBuffer::New(info.Env(), ch_->data(), ch_->byteLength());
  if (napi_create_reference(info.Env(), buffer, 1, &bufferRef_) != napi_ok) {
    Napi::Error::New(info.Env(), "failed to retain event-channel buffer")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  return buffer;
}

Napi::Value Trader::ClaimBatch(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), ch_ ? ch_->claim() : 0);
}

Napi::Value Trader::ReleaseBatch(const Napi::CallbackInfo &info) {
  if (ch_)
    ch_->release(info[0].As<Napi::Number>().Uint32Value());
  return info.Env().Undefined();
}

Napi::Value Trader::DropCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(),
                          static_cast<double>(ch_ ? ch_->dropCount() : 0));
}

Napi::Value Trader::ChannelInfo(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object o = Napi::Object::New(env);
  const bool live = ch_ != nullptr;
  o.Set("slotSize", static_cast<double>(live ? ch_->slotSize() : 0));
  o.Set("numSlots", static_cast<double>(live ? ch_->numSlots() : 0));
  o.Set("headerSize", static_cast<double>(live ? ch_->headerSize() : 0));
  // Trader uses Lossless; this flag is only the MarketData JS drain selector.
  o.Set("dropOldest", false);
  return o;
}

Napi::Value Trader::Close(const Napi::CallbackInfo &info) {
  doClose();
  return info.Env().Undefined();
}

Napi::Function InitTrader(Napi::Env env) { return Trader::Init(env); }

} // namespace ctp
