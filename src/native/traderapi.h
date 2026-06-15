/*
 * traderapi.h - exposes the Trader N-API class constructor.
 */

#ifndef CTP_NATIVE_TRADERAPI_H
#define CTP_NATIVE_TRADERAPI_H

#include <napi.h>

namespace ctp {
Napi::Function InitTrader(Napi::Env env);
}

#endif /* CTP_NATIVE_TRADERAPI_H */
