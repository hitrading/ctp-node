/*
 * mdapi.h - exposes the MarketData N-API class constructor.
 */

#ifndef CTP_NATIVE_MDAPI_H
#define CTP_NATIVE_MDAPI_H

#include <napi.h>

namespace ctp {
Napi::Function InitMarketData(Napi::Env env);
}

#endif /* CTP_NATIVE_MDAPI_H */
