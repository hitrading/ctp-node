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

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../generated/layout.gen.h"
#include "../generated/structids.gen.h"
#include "ThostFtdcMdApi.h"
#include "channel.h"
#include "mdspi.h"

namespace ctp {

namespace {

void getStr(const Napi::Object &o, const char *key, char *dst, size_t cap) {
  if (!o.Has(key))
    return;
  Napi::Value v = o.Get(key);
  if (!v.IsString())
    return;
  std::string s = v.As<Napi::String>().Utf8Value();
  std::snprintf(dst, cap, "%s", s.c_str());
}

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

} // namespace

class MarketData : public Napi::ObjectWrap<MarketData> {
public:
  static Napi::Function Init(Napi::Env env);
  MarketData(const Napi::CallbackInfo &info);
  ~MarketData();

private:
  void doClose();
  int subscribeImpl(const Napi::CallbackInfo &info,
                    int (CThostFtdcMdApi::*fn)(char *[], int));

  Napi::Value GetApiVersion(const Napi::CallbackInfo &info);
  Napi::Value GetTradingDay(const Napi::CallbackInfo &info);
  Napi::Value SubscribeMarketData(const Napi::CallbackInfo &info);
  Napi::Value UnsubscribeMarketData(const Napi::CallbackInfo &info);
  Napi::Value SubscribeForQuoteRsp(const Napi::CallbackInfo &info);
  Napi::Value UnsubscribeForQuoteRsp(const Napi::CallbackInfo &info);
  Napi::Value ReqUserLogin(const Napi::CallbackInfo &info);
  Napi::Value ReqUserLogout(const Napi::CallbackInfo &info);
  Napi::Value Start(const Napi::CallbackInfo &info);
  Napi::Value Buffer(const Napi::CallbackInfo &info);
  Napi::Value ClaimBatch(const Napi::CallbackInfo &info);
  Napi::Value ReleaseBatch(const Napi::CallbackInfo &info);
  Napi::Value DropCount(const Napi::CallbackInfo &info);
  Napi::Value ChannelInfo(const Napi::CallbackInfo &info);
  Napi::Value Close(const Napi::CallbackInfo &info);
  Napi::Value AttachArm(const Napi::CallbackInfo &info);
  Napi::Value InjectTestTick(const Napi::CallbackInfo &info);

  CThostFtdcMdApi *api_ = nullptr;
  MdSpi *spi_ = nullptr;
  EventChannel *ch_ = nullptr;
  std::shared_ptr<ArmRegistry> armReg_;
  bool started_ = false;
  bool closed_ = false;
};

Napi::Function MarketData::Init(Napi::Env env) {
  return DefineClass(
      env, "MarketData",
      {
          InstanceMethod("getApiVersion", &MarketData::GetApiVersion),
          InstanceMethod("getTradingDay", &MarketData::GetTradingDay),
          InstanceMethod("subscribeMarketData", &MarketData::SubscribeMarketData),
          InstanceMethod("unsubscribeMarketData", &MarketData::UnsubscribeMarketData),
          InstanceMethod("subscribeForQuoteRsp", &MarketData::SubscribeForQuoteRsp),
          InstanceMethod("unsubscribeForQuoteRsp", &MarketData::UnsubscribeForQuoteRsp),
          InstanceMethod("reqUserLogin", &MarketData::ReqUserLogin),
          InstanceMethod("reqUserLogout", &MarketData::ReqUserLogout),
          InstanceMethod("_start", &MarketData::Start),
          InstanceMethod("_buffer", &MarketData::Buffer),
          InstanceMethod("_claim", &MarketData::ClaimBatch),
          InstanceMethod("_release", &MarketData::ReleaseBatch),
          InstanceMethod("_dropCount", &MarketData::DropCount),
          InstanceMethod("_info", &MarketData::ChannelInfo),
          InstanceMethod("_attachArm", &MarketData::AttachArm),
          InstanceMethod("_injectTestTick", &MarketData::InjectTestTick),
          InstanceMethod("close", &MarketData::Close),
      });
}

MarketData::MarketData(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<MarketData>(info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsString()) {
    Napi::TypeError::New(env, "MarketData(flowPath, fronts)")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string flowPath = info[0].As<Napi::String>().Utf8Value();

  api_ = CThostFtdcMdApi::CreateFtdcMdApi(flowPath.c_str());
  if (!api_) {
    Napi::Error::New(env, "CreateFtdcMdApi failed").ThrowAsJavaScriptException();
    return;
  }
  ch_ = new EventChannel(8192, static_cast<size_t>(ctpMaxStructSize()));
  spi_ = new MdSpi(api_, ch_);
  api_->RegisterSpi(spi_);

  for (auto &addr : toStringList(info[1]))
    api_->RegisterFront(const_cast<char *>(addr.c_str()));
}

MarketData::~MarketData() {
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

void MarketData::doClose() {
  if (closed_)
    return;
  closed_ = true;
  // Detach the SPI's api pointer BEFORE releasing the api, so a late callback
  // (or the test tick hook) can never dereference the freed api.
  if (spi_) {
    spi_->clearApi();
    spi_->setArmRegistry(nullptr);
  }
  if (api_) {
    api_->Release(); // stops the CTP callback thread (no more push / onTick)
    api_ = nullptr;
  }
  armReg_.reset();
  if (ch_)
    ch_->stop(); // abort doorbell TSF
}

Napi::Value MarketData::GetApiVersion(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), CThostFtdcMdApi::GetApiVersion());
}

Napi::Value MarketData::GetTradingDay(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return env.Null();
  const char *d = api_->GetTradingDay();
  return Napi::String::New(env, d ? d : "");
}

int MarketData::subscribeImpl(const Napi::CallbackInfo &info,
                              int (CThostFtdcMdApi::*fn)(char *[], int)) {
  if (!api_)
    return -1;
  auto ids = toStringList(info[0]);
  if (ids.empty())
    return 0;
  std::vector<char *> ptrs(ids.size());
  for (size_t i = 0; i < ids.size(); ++i)
    ptrs[i] = const_cast<char *>(ids[i].c_str());
  return (api_->*fn)(ptrs.data(), static_cast<int>(ids.size()));
}

Napi::Value MarketData::SubscribeMarketData(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(),
                           subscribeImpl(info, &CThostFtdcMdApi::SubscribeMarketData));
}
Napi::Value MarketData::UnsubscribeMarketData(const Napi::CallbackInfo &info) {
  return Napi::Number::New(
      info.Env(), subscribeImpl(info, &CThostFtdcMdApi::UnSubscribeMarketData));
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
  CThostFtdcReqUserLoginField req;
  std::memset(&req, 0, sizeof(req));
  if (info[0].IsObject()) {
    Napi::Object o = info[0].As<Napi::Object>();
    getStr(o, "tradingDay", req.TradingDay, sizeof(req.TradingDay));
    getStr(o, "brokerId", req.BrokerID, sizeof(req.BrokerID));
    getStr(o, "userId", req.UserID, sizeof(req.UserID));
    getStr(o, "password", req.Password, sizeof(req.Password));
    getStr(o, "userProductInfo", req.UserProductInfo, sizeof(req.UserProductInfo));
  }
  int id = info[1].As<Napi::Number>().Int32Value();
  return Napi::Number::New(env, api_->ReqUserLogin(&req, id));
}

Napi::Value MarketData::ReqUserLogout(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!api_)
    return Napi::Number::New(env, -1);
  CThostFtdcUserLogoutField req;
  std::memset(&req, 0, sizeof(req));
  if (info[0].IsObject()) {
    Napi::Object o = info[0].As<Napi::Object>();
    getStr(o, "brokerId", req.BrokerID, sizeof(req.BrokerID));
    getStr(o, "userId", req.UserID, sizeof(req.UserID));
  }
  int id = info[1].As<Napi::Number>().Int32Value();
  return Napi::Number::New(env, api_->ReqUserLogout(&req, id));
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
  return Napi::ArrayBuffer::New(info.Env(), ch_->data(), ch_->byteLength());
}

Napi::Value MarketData::ClaimBatch(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), ch_->claim());
}

Napi::Value MarketData::ReleaseBatch(const Napi::CallbackInfo &info) {
  ch_->release(info[0].As<Napi::Number>().Uint32Value());
  return info.Env().Undefined();
}

Napi::Value MarketData::DropCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(),
                           static_cast<double>(ch_->dropCount()));
}

Napi::Value MarketData::ChannelInfo(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object o = Napi::Object::New(env);
  o.Set("slotSize", static_cast<double>(ch_->slotSize()));
  o.Set("numSlots", static_cast<double>(ch_->numSlots()));
  o.Set("headerSize", static_cast<double>(ch_->headerSize()));
  return o;
}

Napi::Value MarketData::Close(const Napi::CallbackInfo &info) {
  doClose();
  return info.Env().Undefined();
}

Napi::Value MarketData::AttachArm(const Napi::CallbackInfo &info) {
  auto ext = info[0].As<Napi::External<std::shared_ptr<ArmRegistry>>>();
  std::shared_ptr<ArmRegistry> *sp = ext.Data();
  if (sp && *sp) {
    armReg_ = *sp; // share ownership so the registry can't dangle
    if (spi_)
      spi_->setArmRegistry(armReg_.get());
  }
  return info.Env().Undefined();
}

Napi::Value MarketData::InjectTestTick(const Napi::CallbackInfo &info) {
  // Drive the real SPI path (so arm triggers are evaluated too).
  if (spi_ && !closed_) {
    CThostFtdcDepthMarketDataField f;
    std::memset(&f, 0, sizeof(f));
    std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", "rb2510");
    std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", "SHFE");
    f.LastPrice = 3500.5;
    f.BidPrice1 = 3499.0;
    f.AskPrice1 = 3501.0;
    f.BidVolume1 = 10;
    f.Volume = 12345;
    spi_->OnRtnDepthMarketData(&f);
  }
  return info.Env().Undefined();
}

Napi::Function InitMarketData(Napi::Env env) { return MarketData::Init(env); }

} // namespace ctp
