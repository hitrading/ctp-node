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
#include "channel.h"
#include "ctp_field.h"
#include "order_capability.h"
#include "risk.h"
#include "shared_handle.h"
#include "snapshot.h"

namespace ctp {

namespace {

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
      "closeOrderBurst",      "maxReferenceAgeMs",
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

class Trader : public Napi::ObjectWrap<Trader> {
public:
  static Napi::Function Init(Napi::Env env);
  Trader(const Napi::CallbackInfo &info);
  ~Trader();

private:
  void doClose();
  bool detachBufferForClose();

  Napi::Value GetApiVersion(const Napi::CallbackInfo &info);
  Napi::Value GetTradingDay(const Napi::CallbackInfo &info);
  Napi::Value FrontInfo(const Napi::CallbackInfo &info);
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
  Napi::Value Start(const Napi::CallbackInfo &info);
  Napi::Value Buffer(const Napi::CallbackInfo &info);
  Napi::Value ClaimBatch(const Napi::CallbackInfo &info);
  Napi::Value ReleaseBatch(const Napi::CallbackInfo &info);
  Napi::Value DropCount(const Napi::CallbackInfo &info);
  Napi::Value ChannelInfo(const Napi::CallbackInfo &info);
  Napi::Value Close(const Napi::CallbackInfo &info);
  Napi::Value BurstEvents(const Napi::CallbackInfo &info);
  Napi::Value InjectTestLogin(const Napi::CallbackInfo &info);

  // Non-null only between _start() (CreateFtdcTraderApi + Init) and close():
  // CTP's Release() on an instance that never ran Init() joins a reactor thread
  // that was never started, which crashes on newer glibc (Ubuntu 26.04 / 2.43).
  CThostFtdcTraderApi *api_ = nullptr;
  std::string flowPath_;
  std::vector<std::string> fronts_;
  TraderSpi *spi_ = nullptr;
  EventChannel *ch_ = nullptr;
  RiskEngine risk_;
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
          InstanceMethod("frontInfo", &Trader::FrontInfo),
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
          InstanceMethod("_start", &Trader::Start),
          InstanceMethod("_buffer", &Trader::Buffer),
          InstanceMethod("_claim", &Trader::ClaimBatch),
          InstanceMethod("_release", &Trader::ReleaseBatch),
          InstanceMethod("_dropCount", &Trader::DropCount),
          InstanceMethod("_info", &Trader::ChannelInfo),
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

  // The CTP API itself is created in _start(), immediately before Init() (see
  // api_): an instance that is closed or finalized without ever starting then
  // owns no CTP object, so close() never calls Release() on an un-Init()ed API.
  flowPath_ = std::move(flowPath);
  fronts_ = std::move(fronts);
  // Trader uses a lossless staged overflow: order/trade/query returns must never
  // be silently discarded under backpressure.
  ch_ = new EventChannel(4096, static_cast<size_t>(ctpMaxStructSize()),
                          DropPolicy::Lossless);
  spi_ = new TraderSpi(ch_);
  // Session transitions and instrument metadata feed the order-path checks.
  spi_->setRisk(&risk_);
  // A real Trader is bound to a front from birth: until a login and session()
  // confirm an account, it is in the same state as after a front disconnect.
  // Without this, requests the stateless rules cannot judge would pass before
  // the first connection whenever no local rule is configured.
  risk_.invalidateSession();
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

// The connected front and the per-second limits it grants this session. CTP
// fills the address once connected and the limits once logged in.
Napi::Value Trader::FrontInfo(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  CThostFtdcFrontInfoField front;
  std::memset(&front, 0, sizeof(front));
  if (api_)
    api_->GetFrontInfo(&front);
  const size_t length = std::find(std::begin(front.FrontAddr), std::end(front.FrontAddr), '\0') -
                        std::begin(front.FrontAddr);
  Napi::Object out = Napi::Object::New(env);
  out.Set("frontAddr", Napi::String::New(env, front.FrontAddr, length));
  out.Set("qryFreq", Napi::Number::New(env, front.QryFreq));
  out.Set("ftdPkgFreq", Napi::Number::New(env, front.FTDPkgFreq));
  return out;
}

Napi::Value Trader::Req(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::Number::New(env, -1);
  int methodId = info[0].As<Napi::Number>().Int32Value();
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
      !getFiniteNumber(object, "maxReferenceAgeMs", 3000.0,
                       cfg.maxReferenceAgeMs))
    return env.Undefined();
  if (maxOrderVolume < 0.0 || cfg.maxPriceDeviation < 0.0 ||
      cfg.maxNotional < 0.0 || cfg.maxOrdersPerSec < 0.0 ||
      cfg.orderBurst < 0.0 || cfg.maxCloseOrdersPerSec < 0.0 ||
      cfg.closeOrderBurst < 0.0 ||
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
  // Closes the final send barrier and waits for any already-admitted send.
  risk_.haltAll();
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
  risk_.invalidateSession();
  return info.Env().Undefined();
}

// Test-only: establish a confirmed native session offline. Drives the exact
// same RiskEngine handshake the live path uses (beginSessionTransition ->
// acceptLoginSession, as the SPI OnRspUserLogin would, then confirmSessionAccount
// as the high-level session() does), so an order test can reach a confirmed
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
  // Drives pendingSessionRequestId_ (the session-transition slot);
  // acceptLoginSession clears it immediately, leaving no pending state.
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

Napi::Value Trader::Start(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (started_ || closed_ || !ch_)
    return env.Undefined();
  napi_status st = ch_->start(env, info[0]);
  if (st != napi_ok) {
    Napi::Error::New(env, "failed to start event channel")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  started_ = true; // the channel is live; never start it twice, even on failure below
  // Create, wire and Init() back to back with no failure point in between, so
  // a non-null api_ always means Init() ran and Release() is safe.
  CThostFtdcTraderApi *api =
      CThostFtdcTraderApi::CreateFtdcTraderApi(flowPath_.c_str());
  if (!api) {
    Napi::Error::New(env, "CreateFtdcTraderApi failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  api->RegisterSpi(spi_);
  api->SubscribePublicTopic(THOST_TERT_QUICK);
  api->SubscribePrivateTopic(THOST_TERT_QUICK);
  for (auto &addr : fronts_)
    api->RegisterFront(const_cast<char *>(addr.c_str()));
  api->Init();
  api_ = api;
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
