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
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../generated/layout.gen.h"
#include "../generated/traderreq.gen.h"
#include "../generated/traderspi.gen.h"
#include "ThostFtdcTraderApi.h"
#include "arm.h"
#include "channel.h"
#include "risk.h"

namespace ctp {

namespace {

std::vector<std::string> toStringList(const Napi::Value &v) {
  std::vector<std::string> out;
  if (v.IsArray()) {
    Napi::Array a = v.As<Napi::Array>();
    for (uint32_t i = 0; i < a.Length(); ++i)
      out.push_back(a.Get(i).As<Napi::String>().Utf8Value());
  } else if (v.IsString()) {
    out.push_back(v.As<Napi::String>().Utf8Value());
  }
  return out;
}

int getInt(const Napi::Object &o, const char *k, int d) {
  if (o.Has(k)) {
    Napi::Value v = o.Get(k);
    if (v.IsNumber())
      return v.As<Napi::Number>().Int32Value();
  }
  return d;
}
double getDbl(const Napi::Object &o, const char *k, double d) {
  if (o.Has(k)) {
    Napi::Value v = o.Get(k);
    if (v.IsNumber())
      return v.As<Napi::Number>().DoubleValue();
  }
  return d;
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

  Napi::Value GetApiVersion(const Napi::CallbackInfo &info);
  Napi::Value GetTradingDay(const Napi::CallbackInfo &info);
  Napi::Value Req(const Napi::CallbackInfo &info);
  Napi::Value RiskSet(const Napi::CallbackInfo &info);
  Napi::Value RiskHalt(const Napi::CallbackInfo &info);
  Napi::Value RiskResume(const Napi::CallbackInfo &info);
  Napi::Value LastRiskReason(const Napi::CallbackInfo &info);
  Napi::Value SetRefPrice(const Napi::CallbackInfo &info);
  Napi::Value SetMultiplier(const Napi::CallbackInfo &info);
  Napi::Value SetMaxPositionVolume(const Napi::CallbackInfo &info);
  Napi::Value SetMaxInstrumentCost(const Napi::CallbackInfo &info);
  Napi::Value SetSession(const Napi::CallbackInfo &info);
  Napi::Value RebuildReservations(const Napi::CallbackInfo &info);
  Napi::Value SeedPosition(const Napi::CallbackInfo &info);
  Napi::Value PositionCost(const Napi::CallbackInfo &info);
  Napi::Value ResetPositions(const Napi::CallbackInfo &info);
  Napi::Value ApplyTestTrade(const Napi::CallbackInfo &info);
  Napi::Value ApplyTestOrder(const Napi::CallbackInfo &info);
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

  CThostFtdcTraderApi *api_ = nullptr;
  TraderSpi *spi_ = nullptr;
  EventChannel *ch_ = nullptr;
  RiskEngine risk_;
  std::shared_ptr<ArmRegistry> arm_ = std::make_shared<ArmRegistry>();
  std::atomic<int> armReqId_{1000000};
  bool started_ = false;
  bool closed_ = false;
};

Napi::Function Trader::Init(Napi::Env env) {
  return DefineClass(
      env, "Trader",
      {
          InstanceMethod("getApiVersion", &Trader::GetApiVersion),
          InstanceMethod("getTradingDay", &Trader::GetTradingDay),
          InstanceMethod("_req", &Trader::Req),
          InstanceMethod("riskSet", &Trader::RiskSet),
          InstanceMethod("riskHalt", &Trader::RiskHalt),
          InstanceMethod("riskResume", &Trader::RiskResume),
          InstanceMethod("lastRiskReason", &Trader::LastRiskReason),
          InstanceMethod("setRefPrice", &Trader::SetRefPrice),
          InstanceMethod("setMultiplier", &Trader::SetMultiplier),
          InstanceMethod("setMaxPositionVolume", &Trader::SetMaxPositionVolume),
          InstanceMethod("setMaxInstrumentCost", &Trader::SetMaxInstrumentCost),
          InstanceMethod("setSession", &Trader::SetSession),
          InstanceMethod("rebuildReservations", &Trader::RebuildReservations),
          InstanceMethod("seedPosition", &Trader::SeedPosition),
          InstanceMethod("positionCost", &Trader::PositionCost),
          InstanceMethod("resetPositions", &Trader::ResetPositions),
          InstanceMethod("_applyTestTrade", &Trader::ApplyTestTrade),
          InstanceMethod("_applyTestOrder", &Trader::ApplyTestOrder),
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
          InstanceMethod("close", &Trader::Close),
      });
}

Trader::Trader(const Napi::CallbackInfo &info) : Napi::ObjectWrap<Trader>(info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Trader(flowPath, fronts)")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string flowPath = info[0].As<Napi::String>().Utf8Value();

  // Validate fronts BEFORE allocating anything: RegisterFront("") faults deep
  // inside the CTP DLL, so drop empty addresses and reject an empty result with
  // a clean JS error rather than crashing the process.
  std::vector<std::string> fronts;
  for (auto &addr : toStringList(info[1]))
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
  ch_ = new EventChannel(4096, static_cast<size_t>(ctpMaxStructSize()));
  spi_ = new TraderSpi(api_, ch_);
  spi_->setRisk(&risk_); // fills update the position-cost tracker
  api_->RegisterSpi(spi_);
  arm_->setSink(this); // armed triggers fire orders through this Trader
  api_->SubscribePublicTopic(THOST_TERT_QUICK);
  api_->SubscribePrivateTopic(THOST_TERT_QUICK);

  for (auto &addr : fronts)
    api_->RegisterFront(const_cast<char *>(addr.c_str()));
}

Trader::~Trader() { doClose(); }

void Trader::doClose() {
  if (closed_)
    return;
  closed_ = true;
  // Order matters: clearSink() (under the arm lock) guarantees no fireArmed is
  // running or will start, so the non-atomic api_ below is never read after it
  // is nulled. Do NOT move the api_ teardown before clearSink().
  arm_->clearSink();
  if (api_) {
    api_->Release();
    api_ = nullptr;
  }
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
    delete ch_;
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
  // Deviation check is intentionally skipped (refPrice 0) for armed orders:
  // they fire on fast moves where the limit can legitimately differ from the
  // last print, and silently blocking the armed order would be worse. Volume,
  // notional (multiplier-aware), kill-switch and rate limit still apply.
  RiskVerdict v = risk_.check(f.InstrumentID, f.LimitPrice, 0.0,
                              f.VolumeTotalOriginal);
  if (!v.ok)
    return CTP_RISK_BLOCKED;
  // Position caps (incl. in-flight reservation) apply to armed orders too -
  // safety, not latency-sensitive.
  const bool isOpen = (f.CombOffsetFlag[0] == '0');
  if (isOpen) {
    OpenGate g = risk_.tryReserveOpen(f.OrderRef, f.InstrumentID,
                                      f.Direction == '0', f.LimitPrice,
                                      f.VolumeTotalOriginal);
    if (g == OpenGate::VolumeLimit)
      return CTP_POSITION_VOLUME_LIMIT;
    if (g == OpenGate::CostLimit)
      return CTP_POSITION_LIMIT;
  }
  if (!risk_.allowRate()) {
    if (isOpen)
      risk_.releaseReservation(f.OrderRef);
    return CTP_RATE_LIMITED;
  }
  int sendRc = api->ReqOrderInsert(&f, armReqId_.fetch_add(1));
  if (sendRc != 0 && isOpen)
    risk_.releaseReservation(f.OrderRef); // send failed -> release the reservation
  return sendRc;
}

Napi::Value Trader::ArmRegistryExternal(const Napi::CallbackInfo &info) {
  auto *sp = new std::shared_ptr<ArmRegistry>(arm_);
  return Napi::External<std::shared_ptr<ArmRegistry>>::New(
      info.Env(), sp,
      [](Napi::Env, std::shared_ptr<ArmRegistry> *p) { delete p; });
}

Napi::Value Trader::Arm(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  ArmSpec spec;
  spec.instrumentId = info[0].As<Napi::String>().Utf8Value();
  std::string side = info[1].As<Napi::String>().Utf8Value();
  spec.side = side.empty() ? '0' : side[0];
  spec.triggerPrice = info[2].As<Napi::Number>().DoubleValue();
  Napi::Uint8Array bytes = info[3].As<Napi::Uint8Array>();
  spec.orderTemplate.assign(bytes.Data(), bytes.Data() + bytes.ByteLength());
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

Napi::Value Trader::GetApiVersion(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), CThostFtdcTraderApi::GetApiVersion());
}

Napi::Value Trader::GetTradingDay(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return env.Null();
  const char *d = api_->GetTradingDay();
  return Napi::String::New(env, d ? d : "");
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

Napi::Value Trader::RiskSet(const Napi::CallbackInfo &info) {
  if (info[0].IsObject()) {
    Napi::Object o = info[0].As<Napi::Object>();
    RiskConfig cfg;
    cfg.maxOrderVolume = getInt(o, "maxOrderVolume", 0);
    cfg.maxPriceDeviation = getDbl(o, "maxPriceDeviation", 0.0);
    cfg.maxNotional = getDbl(o, "maxNotional", 0.0);
    cfg.maxOrdersPerSec = getDbl(o, "maxOrdersPerSec", 0.0);
    cfg.orderBurst = getDbl(o, "orderBurst", 0.0);
    cfg.maxPositionCost = getDbl(o, "maxPositionCost", 0.0);
    risk_.configure(cfg);
  }
  return info.Env().Undefined();
}

Napi::Value Trader::RiskHalt(const Napi::CallbackInfo &info) {
  risk_.halt();
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

Napi::Value Trader::SetRefPrice(const Napi::CallbackInfo &info) {
  if (info.Length() >= 2 && info[0].IsString() && info[1].IsNumber()) {
    risk_.setRefPrice(info[0].As<Napi::String>().Utf8Value(),
                      info[1].As<Napi::Number>().DoubleValue());
  }
  return info.Env().Undefined();
}

Napi::Value Trader::SetMultiplier(const Napi::CallbackInfo &info) {
  if (info.Length() >= 2 && info[0].IsString() && info[1].IsNumber()) {
    risk_.setMultiplier(info[0].As<Napi::String>().Utf8Value(),
                        info[1].As<Napi::Number>().DoubleValue());
  }
  return info.Env().Undefined();
}

// (instrumentId, isLong, maxVolume) - per-side open-position lot cap; 0 = off.
Napi::Value Trader::SetMaxPositionVolume(const Napi::CallbackInfo &info) {
  if (info.Length() >= 3 && info[0].IsString() && info[2].IsNumber()) {
    risk_.setMaxPositionVolume(info[0].As<Napi::String>().Utf8Value(),
                               info[1].ToBoolean().Value(),
                               info[2].As<Napi::Number>().DoubleValue());
  }
  return info.Env().Undefined();
}

// (instrumentId, maxCost) - per-instrument open-position cost cap; 0 = off.
Napi::Value Trader::SetMaxInstrumentCost(const Napi::CallbackInfo &info) {
  if (info.Length() >= 2 && info[0].IsString() && info[1].IsNumber()) {
    risk_.setMaxInstrumentCost(info[0].As<Napi::String>().Utf8Value(),
                               info[1].As<Napi::Number>().DoubleValue());
  }
  return info.Env().Undefined();
}

// (frontId, sessionId) - our login session, for reservation keys.
Napi::Value Trader::SetSession(const Napi::CallbackInfo &info) {
  if (info.Length() >= 2 && info[0].IsNumber() && info[1].IsNumber()) {
    risk_.setSession(info[0].As<Napi::Number>().Int32Value(),
                     info[1].As<Napi::Number>().Int32Value());
  }
  return info.Env().Undefined();
}

// (Array<{frontId, sessionId, orderRef, instrumentId, isLong, vol, price}>) -
// rebuild in-flight reservations from CTP's working orders (reqQryOrder).
Napi::Value Trader::RebuildReservations(const Napi::CallbackInfo &info) {
  std::vector<OpenOrderInfo> orders;
  if (info.Length() >= 1 && info[0].IsArray()) {
    Napi::Array arr = info[0].As<Napi::Array>();
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value v = arr.Get(i);
      if (!v.IsObject())
        continue;
      Napi::Object o = v.As<Napi::Object>();
      OpenOrderInfo r;
      r.frontId = o.Get("frontId").ToNumber().Int32Value();
      r.sessionId = o.Get("sessionId").ToNumber().Int32Value();
      r.orderRef = o.Get("orderRef").ToString().Utf8Value();
      r.instrumentId = o.Get("instrumentId").ToString().Utf8Value();
      r.isLong = o.Get("isLong").ToBoolean().Value();
      r.vol = o.Get("vol").ToNumber().DoubleValue();
      r.price = o.Get("price").ToNumber().DoubleValue();
      orders.push_back(std::move(r));
    }
  }
  risk_.rebuildOpenReservations(orders);
  return info.Env().Undefined();
}

// (instrumentId, isLong, volume, openCost) - seed a pre-existing position.
Napi::Value Trader::SeedPosition(const Napi::CallbackInfo &info) {
  if (info.Length() >= 4) {
    risk_.seedPosition(info[0].As<Napi::String>().Utf8Value(),
                       info[1].ToBoolean().Value(),
                       info[2].As<Napi::Number>().DoubleValue(),
                       info[3].As<Napi::Number>().DoubleValue());
  }
  return info.Env().Undefined();
}

Napi::Value Trader::PositionCost(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), risk_.currentPositionCost());
}

Napi::Value Trader::ResetPositions(const Napi::CallbackInfo &info) {
  risk_.resetPositions();
  return info.Env().Undefined();
}

// test-only: (instrumentId, isBuy, isOpen, price, volume)
Napi::Value Trader::ApplyTestTrade(const Napi::CallbackInfo &info) {
  if (info.Length() >= 5) {
    risk_.onTrade(info[0].As<Napi::String>().Utf8Value(),
                  info[1].ToBoolean().Value(), info[2].ToBoolean().Value(),
                  info[3].As<Napi::Number>().DoubleValue(),
                  info[4].As<Napi::Number>().DoubleValue());
  }
  return info.Env().Undefined();
}

// (frontId, sessionId, orderRef, instrumentId, isOpen, isLong, status,
// limitPrice, volTotal, volTraded) - drive the reservation tracker (OnRtnOrder).
Napi::Value Trader::ApplyTestOrder(const Napi::CallbackInfo &info) {
  if (info.Length() >= 10) {
    std::string status = info[6].As<Napi::String>().Utf8Value();
    risk_.onOrderUpdate(info[0].As<Napi::Number>().Int32Value(),
                        info[1].As<Napi::Number>().Int32Value(),
                        info[2].As<Napi::String>().Utf8Value(),
                        info[3].As<Napi::String>().Utf8Value(),
                        info[4].ToBoolean().Value(), info[5].ToBoolean().Value(),
                        status.empty() ? ' ' : status[0],
                        info[7].As<Napi::Number>().DoubleValue(),
                        info[8].As<Napi::Number>().DoubleValue(),
                        info[9].As<Napi::Number>().DoubleValue());
  }
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
  return Napi::ArrayBuffer::New(info.Env(), ch_->data(), ch_->byteLength());
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
  return o;
}

Napi::Value Trader::Close(const Napi::CallbackInfo &info) {
  doClose();
  return info.Env().Undefined();
}

Napi::Function InitTrader(Napi::Env env) { return Trader::Init(env); }

} // namespace ctp
