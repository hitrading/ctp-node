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

#include <cstring>
#include <string>
#include <vector>

#include "../generated/layout.gen.h"
#include "../generated/traderreq.gen.h"
#include "../generated/traderspi.gen.h"
#include "ThostFtdcTraderApi.h"
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

class Trader : public Napi::ObjectWrap<Trader> {
public:
  static Napi::Function Init(Napi::Env env);
  Trader(const Napi::CallbackInfo &info);
  ~Trader();

private:
  void doClose();

  Napi::Value GetApiVersion(const Napi::CallbackInfo &info);
  Napi::Value GetTradingDay(const Napi::CallbackInfo &info);
  Napi::Value Req(const Napi::CallbackInfo &info);
  Napi::Value RiskSet(const Napi::CallbackInfo &info);
  Napi::Value RiskHalt(const Napi::CallbackInfo &info);
  Napi::Value RiskResume(const Napi::CallbackInfo &info);
  Napi::Value Start(const Napi::CallbackInfo &info);
  Napi::Value Buffer(const Napi::CallbackInfo &info);
  Napi::Value ClaimBatch(const Napi::CallbackInfo &info);
  Napi::Value ReleaseBatch(const Napi::CallbackInfo &info);
  Napi::Value DropCount(const Napi::CallbackInfo &info);
  Napi::Value ChannelInfo(const Napi::CallbackInfo &info);
  Napi::Value Close(const Napi::CallbackInfo &info);

  CThostFtdcTraderApi *api_ = nullptr;
  TraderSpi *spi_ = nullptr;
  EventChannel *ch_ = nullptr;
  RiskEngine risk_;
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
          InstanceMethod("_start", &Trader::Start),
          InstanceMethod("_buffer", &Trader::Buffer),
          InstanceMethod("_claim", &Trader::ClaimBatch),
          InstanceMethod("_release", &Trader::ReleaseBatch),
          InstanceMethod("_dropCount", &Trader::DropCount),
          InstanceMethod("_info", &Trader::ChannelInfo),
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

  api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flowPath.c_str());
  if (!api_) {
    Napi::Error::New(env, "CreateFtdcTraderApi failed")
        .ThrowAsJavaScriptException();
    return;
  }
  ch_ = new EventChannel(4096, static_cast<size_t>(ctpMaxStructSize()));
  spi_ = new TraderSpi(api_, ch_);
  api_->RegisterSpi(spi_);
  api_->SubscribePublicTopic(THOST_TERT_QUICK);
  api_->SubscribePrivateTopic(THOST_TERT_QUICK);

  for (auto &addr : toStringList(info[1]))
    api_->RegisterFront(const_cast<char *>(addr.c_str()));
}

Trader::~Trader() {
  doClose();
  if (spi_) {
    delete spi_;
    spi_ = nullptr;
  }
  if (ch_) {
    delete ch_;
    ch_ = nullptr;
  }
}

void Trader::doClose() {
  if (closed_)
    return;
  closed_ = true;
  if (api_) {
    api_->Release();
    api_ = nullptr;
  }
  if (ch_)
    ch_->stop();
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
  return Napi::ArrayBuffer::New(info.Env(), ch_->data(), ch_->byteLength());
}

Napi::Value Trader::ClaimBatch(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), ch_->claim());
}

Napi::Value Trader::ReleaseBatch(const Napi::CallbackInfo &info) {
  ch_->release(info[0].As<Napi::Number>().Uint32Value());
  return info.Env().Undefined();
}

Napi::Value Trader::DropCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), static_cast<double>(ch_->dropCount()));
}

Napi::Value Trader::ChannelInfo(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object o = Napi::Object::New(env);
  o.Set("slotSize", static_cast<double>(ch_->slotSize()));
  o.Set("numSlots", static_cast<double>(ch_->numSlots()));
  o.Set("headerSize", static_cast<double>(ch_->headerSize()));
  return o;
}

Napi::Value Trader::Close(const Napi::CallbackInfo &info) {
  doClose();
  return info.Env().Undefined();
}

Napi::Function InitTrader(Napi::Env env) { return Trader::Init(env); }

} // namespace ctp
