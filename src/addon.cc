/*
 * addon.cc - N-API entry point.
 *
 * Exposes (internal __ helpers are for tests/codec bootstrap):
 *   mdApiVersion() / traderApiVersion()  - toolchain smoke
 *   __layoutData()                       - Int32 layout blob (offsetof truth)
 *   __sampleDepthMarketData()            - a filled struct's raw bytes (codec test)
 *   __riskSelfTest()                     - reserved native hooks self-check
 */

#include <napi.h>

#include <cstdio>
#include <cstring>

#include "ThostFtdcMdApi.h"
#include "ThostFtdcTraderApi.h"
#include "generated/layout.gen.h"
#include "native/arm.h"
#include "native/channel.h"
#include "native/mdapi.h"
#include "native/risk.h"
#include "native/traderapi.h"

namespace {

Napi::Value MdApiVersion(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), CThostFtdcMdApi::GetApiVersion());
}

Napi::Value TraderApiVersion(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), CThostFtdcTraderApi::GetApiVersion());
}

// Packed layout blob: [n, (structSize, fieldCount, (off,size,kind)*)...].
// Offsets/sizes are compiler truth (offsetof/sizeof); JS builds decoders.
Napi::Value LayoutData(const Napi::CallbackInfo &info) {
  size_t len = 0;
  const int32_t *d = ctpLayoutData(&len);
  Napi::Int32Array arr = Napi::Int32Array::New(info.Env(), len);
  std::memcpy(arr.Data(), d, len * sizeof(int32_t));
  return arr;
}

// A DepthMarketData filled with known values, returned as raw bytes - lets the
// JS codec round-trip prove its offsets match the C++ struct layout.
Napi::Value SampleDepthMarketData(const Napi::CallbackInfo &info) {
  CThostFtdcDepthMarketDataField f;
  std::memset(&f, 0, sizeof(f));
  std::snprintf(f.TradingDay, sizeof(f.TradingDay), "%s", "20260615");
  std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", "SHFE");
  std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", "rb2510");
  std::snprintf(f.UpdateTime, sizeof(f.UpdateTime), "%s", "21:00:00");
  f.LastPrice = 3500.5;
  f.BidPrice1 = 3499.0;
  f.AskPrice1 = 3501.0;
  f.BidVolume1 = 10;
  f.AskVolume1 = 20;
  f.Volume = 12345;
  f.UpdateMillisec = 500;
  return Napi::Buffer<uint8_t>::Copy(info.Env(),
                                     reinterpret_cast<uint8_t *>(&f), sizeof(f));
}

Napi::Object Verdict(Napi::Env env, ctp::RiskVerdict v) {
  Napi::Object r = Napi::Object::New(env);
  r.Set("ok", v.ok);
  r.Set("reason", v.reason ? Napi::Value(Napi::String::New(env, v.reason))
                           : env.Null());
  return r;
}

Napi::Value RiskSelfTest(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object out = Napi::Object::New(env);

  ctp::RiskEngine eng;
  ctp::RiskConfig cfg;
  cfg.maxOrderVolume = 5;
  cfg.maxPriceDeviation = 0.02;
  eng.configure(cfg);

  out.Set("normal", Verdict(env, eng.check(3500.0, 3500.0, 1)));
  out.Set("tooBig", Verdict(env, eng.check(3500.0, 3500.0, 10)));
  out.Set("offPrice", Verdict(env, eng.check(3500.0, 3000.0, 1)));
  eng.halt();
  out.Set("halted", Verdict(env, eng.check(3500.0, 3500.0, 1)));

  ctp::RateLimiter rl;
  rl.configure(2.0, 2.0);
  Napi::Object rlo = Napi::Object::New(env);
  rlo.Set("first", rl.tryAcquire());
  rlo.Set("second", rl.tryAcquire());
  rlo.Set("thirdBlocked", !rl.tryAcquire());
  out.Set("rateLimiter", rlo);

  ctp::ArmRegistry reg;
  ctp::ArmSpec spec;
  spec.instrumentId = "rb2510";
  spec.side = '0';
  spec.triggerPrice = 3500.0;
  uint64_t id = reg.arm(spec);
  reg.onTick("rb2510", 3499.0, 3500.0); // no sink -> no fire, just registry
  Napi::Object armo = Napi::Object::New(env);
  armo.Set("armedCount", static_cast<double>(reg.size()));
  armo.Set("disarmed", reg.disarm(id));
  out.Set("arm", armo);

  return out;
}

// Verifies SPSC ring mechanics (claim/release/drop) without the CTP network.
Napi::Value RingSelfTest(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object out = Napi::Object::New(env);

  CThostFtdcDepthMarketDataField f;
  std::memset(&f, 0, sizeof(f));
  std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", "rb2510");
  f.LastPrice = 3500.5;

  ctp::EventChannel ch(4, sizeof(f));
  for (int i = 0; i < 3; ++i) {
    f.Volume = 100 + i;
    ch.push(0x1012, i, 1, 0, "", 34, &f, sizeof(f));
  }
  uint32_t claimed = ch.claim();
  ch.release(claimed);
  uint32_t afterRelease = ch.claim();

  ctp::EventChannel ch2(4, sizeof(f));
  int ok = 0, dropped = 0;
  for (int i = 0; i < 6; ++i)
    (ch2.push(0x1012, i, 1, 0, "", 34, &f, sizeof(f)) ? ok : dropped)++;

  out.Set("claimed", claimed);
  out.Set("afterRelease", afterRelease);
  out.Set("pushedOk", ok);
  out.Set("dropped", dropped);
  out.Set("dropCount", static_cast<double>(ch2.dropCount()));
  out.Set("slotSize", static_cast<double>(ch.slotSize()));
  out.Set("numSlots", static_cast<double>(ch.numSlots()));
  return out;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("mdApiVersion", Napi::Function::New(env, MdApiVersion));
  exports.Set("traderApiVersion", Napi::Function::New(env, TraderApiVersion));
  exports.Set("__layoutData", Napi::Function::New(env, LayoutData));
  exports.Set("__sampleDepthMarketData",
              Napi::Function::New(env, SampleDepthMarketData));
  exports.Set("__riskSelfTest", Napi::Function::New(env, RiskSelfTest));
  exports.Set("__ringSelfTest", Napi::Function::New(env, RingSelfTest));
  exports.Set("MarketData", ctp::InitMarketData(env));
  exports.Set("Trader", ctp::InitTrader(env));
  return exports;
}

} // namespace

NODE_API_MODULE(ctp, Init)
