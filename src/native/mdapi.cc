/*
 * mdapi.cc - MarketData N-API class (ObjectWrap).
 *
 * Low-level surface consumed by src/market-data.ts. The JS wrapper adds the
 * EventEmitter + Promise ergonomics and the binary->plain-object decode.
 *
 * Lifecycle note: Init() is deferred to _start(), which is called by JS only
 * after the drain callback is wired up - so the front-connected event cannot be
 * missed (fixes the constructor-time race in the original binding).
 */

#include "mdapi.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../generated/layout.gen.h"
#include "../generated/structids.gen.h"
#include "ThostFtdcMdApi.h"
#include "channel.h"
#include "gbk.h"
#include "mdspi.h"
#include "shared_handle.h"
#include "snapshot.h"

namespace ctp {

namespace {

// Returned when an MD login/logout cannot be linearized against an in-flight
// session transition (CTP echoes request id 0 on the MD channel, so two
// overlapping transitions cannot be correlated). Mirrors the JS-side risk-reject
// code surfaced by client.ts riskRejectMessage().
constexpr int kSessionTransitionUnavailable = -10009;

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

bool getStr(const Napi::Object &o, const char *key, char *dst, size_t cap) {
  if (!o.Has(key))
    return true;
  Napi::Value v = o.Get(key);
  if (!v.IsString()) {
    Napi::TypeError::New(o.Env(), std::string(key) + " must be a string")
        .ThrowAsJavaScriptException();
    return false;
  }
  std::string s = v.As<Napi::String>().Utf8Value();
  if (s.find('\0') != std::string::npos) {
    Napi::RangeError::New(o.Env(), std::string(key) + " must not contain NUL")
        .ThrowAsJavaScriptException();
    return false;
  }
  std::string encoded = gbkEncode(s.data(), s.size());
  if (cap == 0 || encoded.size() >= cap) {
    Napi::RangeError::New(o.Env(), std::string(key) +
                                      " exceeds CTP field byte capacity")
        .ThrowAsJavaScriptException();
    return false;
  }
  std::memcpy(dst, encoded.data(), encoded.size());
  dst[encoded.size()] = '\0';
  return true;
}

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

} // namespace

class MarketData : public Napi::ObjectWrap<MarketData> {
public:
  static Napi::Function Init(Napi::Env env);
  MarketData(const Napi::CallbackInfo &info);
  ~MarketData();

private:
  void doClose();
  bool detachBufferForClose();
  int subscribeImpl(const Napi::CallbackInfo &info,
                    int (CThostFtdcMdApi::*fn)(char *[], int),
                    std::vector<std::string> *acceptedIds = nullptr);

  Napi::Value GetApiVersion(const Napi::CallbackInfo &info);
  Napi::Value GetTradingDay(const Napi::CallbackInfo &info);
  Napi::Value SubscribeMarketData(const Napi::CallbackInfo &info);
  Napi::Value UnsubscribeMarketData(const Napi::CallbackInfo &info);
  Napi::Value SubscribeForQuoteRsp(const Napi::CallbackInfo &info);
  Napi::Value UnsubscribeForQuoteRsp(const Napi::CallbackInfo &info);
  Napi::Value ReqUserLogin(const Napi::CallbackInfo &info);
  Napi::Value ReqUserLogout(const Napi::CallbackInfo &info);
  Napi::Value ReqQryMulticastInstrument(const Napi::CallbackInfo &info);
  Napi::Value Start(const Napi::CallbackInfo &info);
  Napi::Value Buffer(const Napi::CallbackInfo &info);
  Napi::Value ClaimBatch(const Napi::CallbackInfo &info);
  Napi::Value ReadIndex(const Napi::CallbackInfo &info);
  Napi::Value ReleaseBatch(const Napi::CallbackInfo &info);
  Napi::Value DropCount(const Napi::CallbackInfo &info);
  Napi::Value ChannelInfo(const Napi::CallbackInfo &info);
  Napi::Value Close(const Napi::CallbackInfo &info);
  Napi::Value AttachArm(const Napi::CallbackInfo &info);
  Napi::Value Snapshot(const Napi::CallbackInfo &info);
  Napi::Value Last(const Napi::CallbackInfo &info);
  Napi::Value SnapshotExternal(const Napi::CallbackInfo &info);
  Napi::Value InjectTestTick(const Napi::CallbackInfo &info);
  Napi::Value InjectTestForQuote(const Napi::CallbackInfo &info);
  Napi::Value InjectTestLifecycle(const Napi::CallbackInfo &info);
  Napi::Value AbandonSessionTransition(const Napi::CallbackInfo &info);
  Napi::Value TimeoutSessionTransition(const Napi::CallbackInfo &info);
  Napi::Value BurstTicks(const Napi::CallbackInfo &info);
  Napi::Value ProduceTestTicks(const Napi::CallbackInfo &info);

  CThostFtdcMdApi *api_ = nullptr;
  MdSpi *spi_ = nullptr;
  EventChannel *ch_ = nullptr;
  std::shared_ptr<ArmRegistry> armReg_;
  // Latest-tick cache (LVC). shared_ptr so a Trader can borrow it (trackMarketData)
  // for its in-C++ deviation reference, like the arm registry.
  std::shared_ptr<SnapshotCache> snap_ = std::make_shared<SnapshotCache>();
  napi_env env_ = nullptr;
  napi_ref bufferRef_ = nullptr;
  bool started_ = false;
  bool closed_ = false;
  // test-only background producer thread (doorbell cross-thread stress).
  std::thread burst_;
};

Napi::Function MarketData::Init(Napi::Env env) {
  return DefineClass(
      env, "MarketData",
      {
          InstanceMethod("apiVersion", &MarketData::GetApiVersion),
          InstanceMethod("tradingDay", &MarketData::GetTradingDay),
          InstanceMethod("subscribeMarketData", &MarketData::SubscribeMarketData),
          InstanceMethod("unsubscribeMarketData", &MarketData::UnsubscribeMarketData),
          InstanceMethod("subscribeForQuoteRsp", &MarketData::SubscribeForQuoteRsp),
          InstanceMethod("unsubscribeForQuoteRsp", &MarketData::UnsubscribeForQuoteRsp),
          InstanceMethod("reqUserLogin", &MarketData::ReqUserLogin),
          InstanceMethod("reqUserLogout", &MarketData::ReqUserLogout),
          InstanceMethod("reqQryMulticastInstrument",
                         &MarketData::ReqQryMulticastInstrument),
          InstanceMethod("_start", &MarketData::Start),
          InstanceMethod("_buffer", &MarketData::Buffer),
          InstanceMethod("_claim", &MarketData::ClaimBatch),
          InstanceMethod("_readIndex", &MarketData::ReadIndex),
          InstanceMethod("_release", &MarketData::ReleaseBatch),
          InstanceMethod("_dropCount", &MarketData::DropCount),
          InstanceMethod("_info", &MarketData::ChannelInfo),
          InstanceMethod("_attachArm", &MarketData::AttachArm),
          InstanceMethod("snapshot", &MarketData::Snapshot),
          InstanceMethod("last", &MarketData::Last),
          InstanceMethod("_snapshotExternal", &MarketData::SnapshotExternal),
          InstanceMethod("_injectTestTick", &MarketData::InjectTestTick),
          InstanceMethod("_injectTestForQuote", &MarketData::InjectTestForQuote),
          InstanceMethod("_injectTestLifecycle", &MarketData::InjectTestLifecycle),
          InstanceMethod("_abandonSessionTransition",
                         &MarketData::AbandonSessionTransition),
          InstanceMethod("_timeoutSessionTransition",
                         &MarketData::TimeoutSessionTransition),
          InstanceMethod("_burstTicks", &MarketData::BurstTicks),
          InstanceMethod("_produceTestTicks", &MarketData::ProduceTestTicks),
          InstanceMethod("close", &MarketData::Close),
      });
}

MarketData::MarketData(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<MarketData>(info), env_(info.Env()) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsString()) {
    Napi::TypeError::New(env, "MarketData(flowPath, fronts)")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string flowPath = info[0].As<Napi::String>().Utf8Value();
  if (flowPath.find('\0') != std::string::npos) {
    Napi::RangeError::New(env, "MarketData flowPath must not contain NUL")
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
        env, "MarketData: at least one non-empty front address is required")
        .ThrowAsJavaScriptException();
    return;
  }

  api_ = CThostFtdcMdApi::CreateFtdcMdApi(flowPath.c_str());
  if (!api_) {
    Napi::Error::New(env, "CreateFtdcMdApi failed").ThrowAsJavaScriptException();
    return;
  }
  // Market data drops the OLDEST record under backpressure: a full-market feed
  // wants the freshest quote, not a stale one, when the JS loop falls behind.
  ch_ = new EventChannel(8192, static_cast<size_t>(ctpMaxStructSize()),
                         DropPolicy::Oldest);
  spi_ = new MdSpi(ch_);
  spi_->setSnapshotCache(snap_.get());
  api_->RegisterSpi(spi_);

  for (auto &addr : fronts)
    api_->RegisterFront(const_cast<char *>(addr.c_str()));
}

MarketData::~MarketData() { doClose(); }

bool MarketData::detachBufferForClose() {
  if (!bufferRef_)
    return true;

  // Treat every N-API failure as "still attached". close() may intentionally
  // retain the native channel in that impossible-on-supported-Node fallback,
  // but it must never free backing memory while JS might still reference it.
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
        // Verify the postcondition instead of assuming a successful status made
        // the external ArrayBuffer inaccessible.
        detached = false;
        safeToFreeRing =
            napi_is_detached_arraybuffer(env_, buffer, &detached) == napi_ok &&
            detached;
      }
    }
  }
  // Once detached, retaining the JS object is unnecessary. On the fail-safe
  // branch the channel itself is leaked below, so dropping our reference is
  // also safe even if user code still holds the attached ArrayBuffer.
  if (env_)
    napi_delete_reference(env_, bufferRef_);
  bufferRef_ = nullptr;
  return safeToFreeRing;
}

void MarketData::doClose() {
  if (closed_)
    return;
  closed_ = true;
  // Join any test-only burst producer before tearing the channel down, so it
  // can never push into a freed ring.
  if (burst_.joinable())
    burst_.join();
  if (spi_) {
    spi_->setArmRegistry(nullptr);
    spi_->setSnapshotCache(nullptr);
  }
  if (api_) {
    api_->Release(); // stops the CTP callback thread (no more push / onTick)
    api_ = nullptr;
  }
  // A callback may have loaded the old raw cache pointer immediately before
  // setSnapshotCache(nullptr). Release() joins that producer; clear only after
  // it returns so such an in-flight callback cannot repopulate a closed feed
  // with a deceptively fresh risk-reference quote.
  if (snap_)
    snap_->clear(); // a Trader still holding it now sees an empty cache
  armReg_.reset();
  // Every _buffer() call returns the same externally-backed ArrayBuffer. Detach
  // it before freeing the ring so a caller that retained the ArrayBuffer (or a
  // typed view over it) observes byteLength 0 instead of a dangling pointer.
  const bool safeToFreeRing = detachBufferForClose();
  // Free the SPI and the ring here, on close(), rather than waiting for the JS
  // object's GC finalizer: the ring is large (~21 MB - maxStructSize * 8192
  // slots) but invisible to V8, so the GC feels no pressure and a create/close
  // loop balloons RSS (measured ~32 MB/cycle for MD+Trader, unreclaimed even by
  // a forced GC) before finalizers ever run. The CTP producer thread is stopped
  // (Release above) and the burst thread is joined, so nothing can push anymore;
  // the doorbell is aborted in ~EventChannel via stop(). Accessors null-check.
  if (spi_) {
    delete spi_;
    spi_ = nullptr;
  }
  if (ch_) {
    ch_->stop(); // abort doorbell TSF (idempotent)
    if (safeToFreeRing)
      delete ch_;
    // If detachment failed, intentionally leak the stopped channel and its
    // ring. This is the only fail-safe outcome that cannot create a dangling
    // external ArrayBuffer.
    ch_ = nullptr;
  }
}

Napi::Value MarketData::GetApiVersion(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), CThostFtdcMdApi::GetApiVersion());
}

Napi::Value MarketData::GetTradingDay(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::String::New(env, "");
  const char *d = api_->GetTradingDay();
  return Napi::String::New(env, d ? d : "");
}

int MarketData::subscribeImpl(const Napi::CallbackInfo &info,
                              int (CThostFtdcMdApi::*fn)(char *[], int),
                              std::vector<std::string> *acceptedIds) {
  if (!api_)
    return -1;
  std::vector<std::string> ids;
  if (!toStringList(info[0], ids, "instrumentIds"))
    return -1;
  if (ids.empty())
    return 0;
  std::vector<char *> ptrs(ids.size());
  for (size_t i = 0; i < ids.size(); ++i)
    ptrs[i] = const_cast<char *>(ids[i].c_str());
  const int rc = (api_->*fn)(ptrs.data(), static_cast<int>(ids.size()));
  if (rc == 0 && acceptedIds)
    *acceptedIds = std::move(ids);
  return rc;
}

Napi::Value MarketData::SubscribeMarketData(const Napi::CallbackInfo &info) {
  std::vector<std::string> ids;
  const int rc = subscribeImpl(info, &CThostFtdcMdApi::SubscribeMarketData,
                               &ids);
  if (rc == 0 && spi_)
    spi_->noteSubscriptionRequest(ids, true);
  return Napi::Number::New(info.Env(), rc);
}
Napi::Value MarketData::UnsubscribeMarketData(const Napi::CallbackInfo &info) {
  std::vector<std::string> ids;
  const int rc = subscribeImpl(info, &CThostFtdcMdApi::UnSubscribeMarketData,
                               &ids);
  if (rc == 0 && spi_)
    spi_->noteSubscriptionRequest(ids, false);
  return Napi::Number::New(info.Env(), rc);
}
Napi::Value MarketData::SubscribeForQuoteRsp(const Napi::CallbackInfo &info) {
  return Napi::Number::New(
      info.Env(), subscribeImpl(info, &CThostFtdcMdApi::SubscribeForQuoteRsp));
}
Napi::Value MarketData::UnsubscribeForQuoteRsp(const Napi::CallbackInfo &info) {
  return Napi::Number::New(
      info.Env(), subscribeImpl(info, &CThostFtdcMdApi::UnSubscribeForQuoteRsp));
}

Napi::Value MarketData::ReqUserLogin(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::Number::New(env, -1);
  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(
        env, "reqUserLogin requires encoded ReqUserLogin bytes and request id")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::TypedArray typed = info[0].As<Napi::TypedArray>();
  if (typed.TypedArrayType() != napi_uint8_array) {
    Napi::TypeError::New(env, "reqUserLogin bytes must be a Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Uint8Array bytes = info[0].As<Napi::Uint8Array>();
  if (bytes.ByteLength() != sizeof(CThostFtdcReqUserLoginField)) {
    Napi::RangeError::New(env, "invalid encoded ReqUserLogin byte length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CThostFtdcReqUserLoginField req;
  std::memcpy(&req, bytes.Data(), sizeof(req));
  const int id = info[1].As<Napi::Number>().Int32Value();
  if (!spi_ || !spi_->beginLogin(id))
    return Napi::Number::New(env, kSessionTransitionUnavailable);
  const int rc = api_->ReqUserLogin(&req, id);
  if (rc != 0)
    spi_->invalidateSession(); // no callback is attributable to this failed send
  return Napi::Number::New(env, rc);
}

Napi::Value MarketData::ReqUserLogout(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::Number::New(env, -1);
  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(
        env, "reqUserLogout requires encoded UserLogout bytes and request id")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::TypedArray typed = info[0].As<Napi::TypedArray>();
  if (typed.TypedArrayType() != napi_uint8_array) {
    Napi::TypeError::New(env, "reqUserLogout bytes must be a Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Uint8Array bytes = info[0].As<Napi::Uint8Array>();
  if (bytes.ByteLength() != sizeof(CThostFtdcUserLogoutField)) {
    Napi::RangeError::New(env, "invalid encoded UserLogout byte length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CThostFtdcUserLogoutField req;
  std::memcpy(&req, bytes.Data(), sizeof(req));
  const int id = info[1].As<Napi::Number>().Int32Value();
  if (!spi_ || !spi_->beginLogout(id))
    return Napi::Number::New(env, kSessionTransitionUnavailable);
  const int rc = api_->ReqUserLogout(&req, id);
  if (rc != 0)
    spi_->invalidateSession();
  return Napi::Number::New(env, rc);
}

Napi::Value MarketData::ReqQryMulticastInstrument(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::Number::New(env, -1);
  if (info.Length() < 2 || !info[0].IsObject() || info[0].IsArray() ||
      !info[1].IsNumber()) {
    Napi::TypeError::New(env,
                         "reqQryMulticastInstrument requires an object and request id")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CThostFtdcQryMulticastInstrumentField req;
  std::memset(&req, 0, sizeof(req));
  Napi::Object o = info[0].As<Napi::Object>();
  if (o.Has("topicId")) {
    Napi::Value topic = o.Get("topicId");
    if (!topic.IsNumber() ||
        !std::isfinite(topic.As<Napi::Number>().DoubleValue()) ||
        std::floor(topic.As<Napi::Number>().DoubleValue()) !=
            topic.As<Napi::Number>().DoubleValue() ||
        topic.As<Napi::Number>().DoubleValue() < -2147483648.0 ||
        topic.As<Napi::Number>().DoubleValue() > 2147483647.0) {
      Napi::TypeError::New(env, "topicId must be an int32")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    req.TopicID = topic.As<Napi::Number>().Int32Value();
  }
  if (!getStr(o, "instrumentId", req.InstrumentID,
              sizeof(req.InstrumentID)))
    return env.Undefined();
  const double requestId = info[1].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(requestId) || std::floor(requestId) != requestId ||
      requestId < 1.0 || requestId > 2147483647.0) {
    Napi::TypeError::New(env, "request id must be a positive int32")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(
      env, api_->ReqQryMulticastInstrument(&req,
                                           static_cast<int>(requestId)));
}

Napi::Value MarketData::Start(const Napi::CallbackInfo &info) {
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

Napi::Value MarketData::Buffer(const Napi::CallbackInfo &info) {
  if (!ch_)
    return info.Env().Undefined();
  if (bufferRef_) {
    napi_value cached = nullptr;
    if (napi_get_reference_value(info.Env(), bufferRef_, &cached) == napi_ok &&
        cached != nullptr)
      return Napi::Value(info.Env(), cached);
    // Never overwrite a live reference handle with a second external
    // ArrayBuffer. If retrieval fails, fail closed and preserve the original
    // backing-object relationship for doClose() to handle safely.
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

Napi::Value MarketData::ClaimBatch(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), ch_ ? ch_->claim() : 0);
}

Napi::Value MarketData::ReadIndex(const Napi::CallbackInfo &info) {
  return Napi::Number::New(
      info.Env(), static_cast<double>(ch_ ? ch_->readIndex() : 0));
}

Napi::Value MarketData::ReleaseBatch(const Napi::CallbackInfo &info) {
  if (ch_)
    ch_->release(info[0].As<Napi::Number>().Uint32Value());
  return info.Env().Undefined();
}

Napi::Value MarketData::DropCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(
      info.Env(), static_cast<double>(ch_ ? ch_->dropCount() : 0));
}

Napi::Value MarketData::ChannelInfo(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object o = Napi::Object::New(env);
  const bool live = ch_ != nullptr;
  o.Set("slotSize", static_cast<double>(live ? ch_->slotSize() : 0));
  o.Set("numSlots", static_cast<double>(live ? ch_->numSlots() : 0));
  o.Set("headerSize", static_cast<double>(live ? ch_->headerSize() : 0));
  o.Set("dropOldest", live ? ch_->dropOldest() : false);
  return o;
}

Napi::Value MarketData::Close(const Napi::CallbackInfo &info) {
  doClose();
  return info.Env().Undefined();
}

Napi::Value MarketData::AttachArm(const Napi::CallbackInfo &info) {
  if (closed_ || !spi_) {
    Napi::Error::New(info.Env(), "cannot attach arms to a closed MarketData")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  if (info.Length() < 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(info.Env(), "invalid Trader arm-registry handle")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  auto ext = info[0].As<Napi::External<SharedHandle>>();
  SharedHandle *handle = ext.Data();
  if (!handle || handle->magic != SharedHandle::kMagic ||
      handle->kind != SharedHandleKind::ArmRegistry || !handle->arm) {
    Napi::TypeError::New(info.Env(), "invalid Trader arm-registry handle")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  const std::shared_ptr<ArmRegistry> &registry = handle->arm;
  {
    if (!registry->hasSink()) {
      Napi::Error::New(info.Env(), "cannot attach a closed Trader arm registry")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    if (armReg_ && armReg_.get() != registry.get() && armReg_->hasSink()) {
      Napi::Error::New(
          info.Env(),
          "MarketData is already attached to another live Trader arm registry")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    armReg_ = registry; // share ownership so the registry can't dangle
    if (spi_)
      spi_->setArmRegistry(armReg_);
  }
  return info.Env().Undefined();
}

// snapshot(instrumentId) -> the latest full depth tick as raw struct bytes (JS
// decodes with the shared codec, identical to a streamed tick), or null if none.
Napi::Value MarketData::Snapshot(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString())
    return env.Null();
  CThostFtdcDepthMarketDataField f;
  if (!snap_->get(info[0].As<Napi::String>().Utf8Value(), f))
    return env.Null();
  return Napi::Buffer<uint8_t>::Copy(env, reinterpret_cast<uint8_t *>(&f), sizeof(f));
}

// last(instrumentId) -> latest price (0 if none). Synchronous, no event wait.
Napi::Value MarketData::Last(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString())
    return Napi::Number::New(env, 0.0);
  return Napi::Number::New(env, snap_->last(info[0].As<Napi::String>().Utf8Value()));
}

// Hand the LVC (as a shared_ptr) to a Trader so its deviation check reads live
// prices in C++ (Trader._attachSnapshot). Mirrors the arm-registry sharing: a
// heap shared_ptr copy with a finalizer, so the cache can't dangle.
Napi::Value MarketData::SnapshotExternal(const Napi::CallbackInfo &info) {
  auto *handle = new SharedHandle();
  handle->kind = SharedHandleKind::SnapshotCache;
  handle->snapshot = snap_;
  return Napi::External<SharedHandle>::New(
      info.Env(), handle, [](Napi::Env, SharedHandle *p) { delete p; });
}

Napi::Value MarketData::InjectTestTick(const Napi::CallbackInfo &info) {
  if (!testHooksEnabled(info.Env())) {
    Napi::Error::New(info.Env(),
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  // Drive the real SPI path (so arm triggers are evaluated too). Optional args
  // override bid/ask, so tests can inject edge prices - e.g. a DBL_MAX "no-bid"
  // sentinel (BidPrice1 at limit-down) to verify armed triggers ignore it:
  //   _injectTestTick(bid?, ask?)
  if (spi_ && !closed_) {
    CThostFtdcDepthMarketDataField f;
    std::memset(&f, 0, sizeof(f));
    std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", "rb2510");
    std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", "SHFE");
    f.LastPrice = 3500.5;
    f.BidPrice1 = (info.Length() >= 1 && info[0].IsNumber())
                      ? info[0].As<Napi::Number>().DoubleValue()
                      : 3499.0;
    f.AskPrice1 = (info.Length() >= 2 && info[1].IsNumber())
                      ? info[1].As<Napi::Number>().DoubleValue()
                      : 3501.0;
    f.BidVolume1 = 10;
    f.Volume = 12345;
    spi_->OnRtnDepthMarketData(&f);
  }
  return info.Env().Undefined();
}

Napi::Value MarketData::InjectTestForQuote(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!testHooksEnabled(env)) {
    Napi::Error::New(env,
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (spi_ && !closed_) {
    CThostFtdcForQuoteRspField response;
    std::memset(&response, 0, sizeof(response));
    std::snprintf(response.TradingDay, sizeof(response.TradingDay), "%s",
                  "20260711");
    std::snprintf(response.InstrumentID, sizeof(response.InstrumentID), "%s",
                  "rb2510");
    std::snprintf(response.ExchangeID, sizeof(response.ExchangeID), "%s",
                  "SHFE");
    spi_->OnRtnForQuoteRsp(&response);
  }
  return env.Undefined();
}

Napi::Value MarketData::AbandonSessionTransition(
    const Napi::CallbackInfo &info) {
  if (spi_ && !closed_)
    spi_->abandonSessionTransition();
  return info.Env().Undefined();
}

Napi::Value MarketData::TimeoutSessionTransition(
    const Napi::CallbackInfo &info) {
  if (!spi_ || closed_ || info.Length() < 1 || !info[0].IsNumber())
    return Napi::Boolean::New(info.Env(), true);
  const double raw = info[0].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(raw) || std::floor(raw) != raw || raw <= 0.0 ||
      raw > 2147483647.0)
    return Napi::Boolean::New(info.Env(), true);
  return Napi::Boolean::New(
      info.Env(),
      spi_->timeoutSessionTransition(static_cast<int>(raw)));
}

// Test-only lifecycle driver. It calls the real SPI callbacks so snapshot
// validity, control-event publication and JS request-context listeners are
// exercised together rather than mutating SnapshotCache behind the SPI.
Napi::Value MarketData::InjectTestLifecycle(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!testHooksEnabled(env)) {
    Napi::Error::New(env,
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (closed_ || !spi_)
    return env.Undefined();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "test lifecycle state must be a string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const std::string state = info[0].As<Napi::String>().Utf8Value();
  if (state == "connected") {
    spi_->OnFrontConnected();
  } else if (state == "disconnected") {
    spi_->OnFrontDisconnected(0x1001);
  } else if (state == "login") {
    CThostFtdcRspUserLoginField response;
    std::memset(&response, 0, sizeof(response));
    std::snprintf(response.TradingDay, sizeof(response.TradingDay), "%s",
                  "20260711");
    std::snprintf(response.BrokerID, sizeof(response.BrokerID), "%s", "9999");
    std::snprintf(response.UserID, sizeof(response.UserID), "%s", "test");
    if (spi_->beginLogin(9001))
      spi_->OnRspUserLogin(&response, nullptr, 0, true);
  } else if (state == "login-error") {
    CThostFtdcRspInfoField error;
    std::memset(&error, 0, sizeof(error));
    error.ErrorID = 1;
    std::snprintf(error.ErrorMsg, sizeof(error.ErrorMsg), "%s", "test login error");
    if (spi_->beginLogin(9002))
      spi_->OnRspUserLogin(nullptr, &error, 0, true);
  } else if (state == "logout") {
    CThostFtdcUserLogoutField response;
    std::memset(&response, 0, sizeof(response));
    std::snprintf(response.BrokerID, sizeof(response.BrokerID), "%s", "9999");
    std::snprintf(response.UserID, sizeof(response.UserID), "%s", "test");
    if (spi_->beginLogout(9003))
      spi_->OnRspUserLogout(&response, nullptr, 0, true);
  } else if (state == "login-pending") {
    (void)spi_->beginLogin(9004);
  } else if (state == "abandon") {
    spi_->abandonSessionTransition();
  } else if (state == "late-login") {
    CThostFtdcRspUserLoginField response;
    std::memset(&response, 0, sizeof(response));
    std::snprintf(response.TradingDay, sizeof(response.TradingDay), "%s",
                  "20260711");
    std::snprintf(response.BrokerID, sizeof(response.BrokerID), "%s", "9999");
    std::snprintf(response.UserID, sizeof(response.UserID), "%s", "late");
    spi_->OnRspUserLogin(&response, nullptr, 0, true);
  } else {
    Napi::RangeError::New(
        env,
        "test lifecycle state must be connected, disconnected, login, login-error, logout, login-pending, abandon or late-login")
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

// test-only: spawn a background producer thread that pushes `n` synthetic depth
// ticks straight into the real channel (the exact cross-thread SPSC + coalesced
// doorbell path), each tagged with its sequence number in the requestId slot so
// JS can verify conservation (received-unique + dropped == n) and detect any
// lost wakeup (a stranded tail that never wakes the JS drain). This is the
// doorbell race the claim() seq_cst fence guards.
Napi::Value MarketData::BurstTicks(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!testHooksEnabled(env)) {
    Napi::Error::New(env,
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (closed_ || !ch_)
    return env.Undefined();
  if (burst_.joinable())
    burst_.join(); // only one burst at a time
  int n = info.Length() >= 1 ? info[0].As<Napi::Number>().Int32Value() : 0;
  // optional inter-push jitter in microseconds (0 = none) to widen the window.
  int jitterUs = info.Length() >= 2 ? info[1].As<Napi::Number>().Int32Value() : 0;
  const bool injectControl =
      info.Length() >= 3 && info[2].ToBoolean().Value();
  EventChannel *ch = ch_;
  burst_ = std::thread([ch, n, jitterUs, injectControl]() {
    CThostFtdcDepthMarketDataField f;
    std::memset(&f, 0, sizeof(f));
    std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", "rb2510");
    std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", "SHFE");
    f.LastPrice = 3500.5;
    for (int i = 0; i < n; ++i) {
      if (injectControl && i == n / 2) {
        int marker = 123;
        ch->push(MD_HEARTBEAT_WARNING, 0, -1, 0, "", -2, &marker,
                 sizeof(marker)); // non-droppable control callback
      }
      f.Volume = i; // sequence echo in the payload too
      // requestId field carries the sequence number (JS reads it per record).
      ch->push(MD_RTN_DEPTH_MARKET_DATA, i, -1, 0, "", SID_DepthMarketData, &f,
               (int)sizeof(f), true);
      if (jitterUs > 0 && (i & 0x3F) == 0)
        std::this_thread::sleep_for(std::chrono::microseconds(jitterUs));
    }
  });
  return env.Undefined();
}

// Test-only full hot-path producer. Unlike _burstTicks (which intentionally
// targets EventChannel directly), this calls MdSpi::OnRtnDepthMarketData from a
// native background thread. Consequently it exercises TradingDay enrichment,
// SnapshotCache cardinality, arm lookup, channel overflow and JS decoding with
// distinct InstrumentID values.
//
//   _produceTestTicks(count, instrumentCount, ratePerSecond = 0,
//                     burstSize = 512)
//
// ratePerSecond=0 emits one instantaneous burst. A positive rate emits
// burstSize-tick micro-bursts on a steady-clock schedule. Volume carries the
// zero-based global sequence; InstrumentID rotates SIM00000000..N-1.
Napi::Value MarketData::ProduceTestTicks(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!testHooksEnabled(env)) {
    Napi::Error::New(env,
                     "test hook disabled; set CTP_NODE_TEST_HOOKS=1 explicitly")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (closed_ || !spi_)
    return env.Undefined();

  auto boundedInteger = [&](size_t index, const char *name, int minimum,
                            int maximum, int fallback, int &out) -> bool {
    if (info.Length() <= index) {
      out = fallback;
      return true;
    }
    if (!info[index].IsNumber()) {
      Napi::TypeError::New(env, std::string(name) + " must be an integer")
          .ThrowAsJavaScriptException();
      return false;
    }
    const double value = info[index].As<Napi::Number>().DoubleValue();
    if (!std::isfinite(value) || std::floor(value) != value ||
        value < minimum || value > maximum) {
      Napi::RangeError::New(
          env, std::string(name) + " must be an integer in " +
                   std::to_string(minimum) + ".." + std::to_string(maximum))
          .ThrowAsJavaScriptException();
      return false;
    }
    out = static_cast<int>(value);
    return true;
  };

  int count = 0;
  int instrumentCount = 0;
  int ratePerSecond = 0;
  int burstSize = 512;
  if (!boundedInteger(0, "count", 0, 100000000, 0, count) ||
      !boundedInteger(1, "instrumentCount", 1, 1000000, 1,
                      instrumentCount) ||
      !boundedInteger(2, "ratePerSecond", 0, 100000000, 0,
                      ratePerSecond) ||
      !boundedInteger(3, "burstSize", 1, 1000000, 512, burstSize))
    return env.Undefined();

  if (burst_.joinable())
    burst_.join(); // one native test producer at a time
  MdSpi *spi = spi_;
  burst_ = std::thread(
      [spi, count, instrumentCount, ratePerSecond, burstSize]() {
        CThostFtdcDepthMarketDataField tick;
        std::memset(&tick, 0, sizeof(tick));
        std::snprintf(tick.ExchangeID, sizeof(tick.ExchangeID), "%s", "SIM");
        std::snprintf(tick.ActionDay, sizeof(tick.ActionDay), "%s", "20260711");
        std::snprintf(tick.UpdateTime, sizeof(tick.UpdateTime), "%s", "09:30:00");
        tick.BidVolume1 = 10;
        tick.AskVolume1 = 11;

        // Build the synthetic contract universe once. Per-tick snprintf would
        // measure string formatting in the generator rather than the binding's
        // SPI/cache/channel hot path and substantially inflate reported CPU.
        using InstrumentBytes =
            std::array<char, sizeof(CThostFtdcDepthMarketDataField::InstrumentID)>;
        std::vector<InstrumentBytes> instrumentIds(
            static_cast<size_t>(instrumentCount));
        std::vector<double> prices(static_cast<size_t>(instrumentCount));
        for (int instrument = 0; instrument < instrumentCount; ++instrument) {
          auto &bytes = instrumentIds[static_cast<size_t>(instrument)];
          std::snprintf(bytes.data(), bytes.size(), "SIM%08d", instrument);
          prices[static_cast<size_t>(instrument)] =
              3500.0 + static_cast<double>(instrument % 100) * 0.2;
        }

        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) {
          // Pace the start of each micro-burst. If SPI/JS work is already slower
          // than the target, sleep_until returns immediately rather than adding
          // delay or creating catch-up timer drift.
          if (ratePerSecond > 0 && i > 0 && i % burstSize == 0) {
            const auto target = started + std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(i) / ratePerSecond));
            std::this_thread::sleep_until(target);
          }

          const int instrument = i % instrumentCount;
          std::memcpy(
              tick.InstrumentID,
              instrumentIds[static_cast<size_t>(instrument)].data(),
              sizeof(tick.InstrumentID));
          tick.LastPrice = prices[static_cast<size_t>(instrument)];
          tick.BidPrice1 = tick.LastPrice - 0.2;
          tick.AskPrice1 = tick.LastPrice + 0.2;
          tick.Volume = i; // zero-based sequence for loss/order verification
          tick.UpdateMillisec = i % 1000;
          spi->OnRtnDepthMarketData(&tick);
        }
      });
  return env.Undefined();
}

Napi::Function InitMarketData(Napi::Env env) { return MarketData::Init(env); }

} // namespace ctp
