/*
 * channel.cc - see channel.h
 */

#include "channel.h"

#include <algorithm>
#include <cstring>

namespace ctp {

static inline size_t align8(size_t n) { return (n + 7) & ~static_cast<size_t>(7); }
static inline void putI32(uint8_t *p, int32_t v) { std::memcpy(p, &v, sizeof(v)); }

EventChannel::EventChannel(size_t numSlots, size_t maxPayload)
    : numSlots_(numSlots == 0 ? 1 : numSlots),
      slotSize_(align8(SLOT_HEADER_SIZE + maxPayload)),
      maxPayload_(maxPayload),
      ring_(slotSize_ * numSlots_, 0) {}

EventChannel::~EventChannel() { stop(); }

napi_status EventChannel::start(napi_env env, napi_value drainCb) {
  napi_value name;
  napi_status st =
      napi_create_string_utf8(env, "ctp-doorbell", NAPI_AUTO_LENGTH, &name);
  if (st != napi_ok)
    return st;
  return napi_create_threadsafe_function(env, drainCb, nullptr, name, 0, 1,
                                         nullptr, nullptr, this, onDoorbell,
                                         &tsfn_);
}

void EventChannel::stop() {
  if (tsfn_) {
    napi_release_threadsafe_function(tsfn_, napi_tsfn_abort);
    tsfn_ = nullptr;
  }
}

bool EventChannel::push(int32_t eventType, int32_t requestId, int32_t isLast,
                        int32_t errorId, const char *errorMsg, int32_t structId,
                        const void *payload, int32_t payloadLen) {
  const uint64_t w = writeIdx_.load(std::memory_order_relaxed);
  const uint64_t r = readIdx_.load(std::memory_order_acquire);
  if (w - r >= numSlots_) {
    drops_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint8_t *slot = ring_.data() + (w % numSlots_) * slotSize_;
  putI32(slot + SLOT_EVENT_TYPE, eventType);
  putI32(slot + SLOT_REQUEST_ID, requestId);
  putI32(slot + SLOT_IS_LAST, isLast);
  putI32(slot + SLOT_ERROR_ID, errorId);
  putI32(slot + SLOT_STRUCT_ID, structId);

  if (errorMsg && errorMsg[0]) {
    std::strncpy(reinterpret_cast<char *>(slot + SLOT_ERROR_MSG), errorMsg,
                 SLOT_ERROR_MSG_CAP - 1);
    slot[SLOT_ERROR_MSG + SLOT_ERROR_MSG_CAP - 1] = 0;
  } else {
    slot[SLOT_ERROR_MSG] = 0;
  }

  int32_t n = 0;
  if (payload && payloadLen > 0) {
    n = std::min<int32_t>(payloadLen, static_cast<int32_t>(maxPayload_));
    std::memcpy(slot + SLOT_HEADER_SIZE, payload, static_cast<size_t>(n));
  }
  putI32(slot + SLOT_PAYLOAD_LEN, n);

  writeIdx_.store(w + 1, std::memory_order_release);

  if (!pending_.exchange(true, std::memory_order_acq_rel) && tsfn_) {
    napi_call_threadsafe_function(tsfn_, nullptr, napi_tsfn_nonblocking);
  }
  return true;
}

uint32_t EventChannel::claim() {
  pending_.store(false, std::memory_order_release);
  // Full fence so clearing pending_ is ordered BEFORE reading writeIdx_ (a
  // release store + acquire load on different atomics does NOT order StoreLoad,
  // and x86 reorders it). Without this, a concurrent push could both observe
  // pending_ still true (skip the doorbell) AND publish a writeIdx this load
  // misses -> a stranded record (lost wakeup). The producer's exchange() is a
  // locked RMW (already a full barrier), so only the consumer needs this.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const uint64_t w = writeIdx_.load(std::memory_order_acquire);
  const uint64_t r = readIdx_.load(std::memory_order_relaxed);
  return static_cast<uint32_t>(w - r);
}

void EventChannel::release(uint32_t n) {
  readIdx_.store(readIdx_.load(std::memory_order_relaxed) + n,
                 std::memory_order_release);
}

void EventChannel::onDoorbell(napi_env env, napi_value jsCb, void *ctx,
                              void *data) {
  (void)ctx;
  (void)data;
  if (env == nullptr || jsCb == nullptr)
    return; // teardown drain
  napi_value undef;
  napi_get_undefined(env, &undef);
  napi_call_function(env, undef, jsCb, 0, nullptr, nullptr);
}

} // namespace ctp
