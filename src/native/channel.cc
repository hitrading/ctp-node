/*
 * channel.cc - see channel.h
 */

#include "channel.h"

#include <algorithm>
#include <cstring>

namespace ctp {

static inline size_t align8(size_t n) { return (n + 7) & ~static_cast<size_t>(7); }
static inline void putI32(uint8_t *p, int32_t v) { std::memcpy(p, &v, sizeof(v)); }

EventChannel::EventChannel(size_t numSlots, size_t maxPayload, DropPolicy policy)
    : numSlots_(numSlots == 0 ? 1 : numSlots),
      slotSize_(align8(SLOT_HEADER_SIZE + maxPayload)),
      maxPayload_(maxPayload),
      policy_(policy),
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
  if (policy_ == DropPolicy::Newest) {
    // Reliable: refuse (and count) the newest record when full so nothing
    // already queued is lost. Only the producer touches drops_ in this mode.
    const uint64_t r = readIdx_.load(std::memory_order_acquire);
    if (w - r >= numSlots_) {
      drops_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  // DropPolicy::Oldest: never refuse. When full this overwrites the slot holding
  // the oldest unread record (slot (w % numSlots) == slot (readIdx % numSlots)
  // once w - readIdx == numSlots). The producer stays wait-free and deliberately
  // does NOT read/advance readIdx_ or touch drops_ - doing so from the producer
  // would race the consumer, the sole writer of both. The consumer reconciles
  // the overrun in claim() (skip lapped records) and validate() (discard a read
  // torn mid-overwrite), and owns the drop counter.

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
  uint64_t r = readIdx_.load(std::memory_order_relaxed);
  // DropPolicy::Oldest: if the producer lapped us while we were idle, the oldest
  // (w - r) - numSlots records have been overwritten and are gone. Skip them so
  // we only hand the consumer the freshest numSlots slots. Counting + advancing
  // readIdx_ here is safe because the consumer is the sole writer of both readIdx_
  // and (in this mode) drops_. Under DropPolicy::Newest the producer keeps
  // w - r <= numSlots, so this branch never fires.
  if (w - r > numSlots_) {
    const uint64_t skip = (w - r) - numSlots_;
    drops_.fetch_add(skip, std::memory_order_relaxed);
    r += skip;
    readIdx_.store(r, std::memory_order_release);
  }
  return static_cast<uint32_t>(w - r);
}

uint32_t EventChannel::validate(uint32_t claimed) {
  // Consumer thread. The producer may have lapped the ring while we decoded the
  // batch claim() handed us, overwriting the oldest records mid-read. Compare the
  // current write index against the batch start (readIdx_, unchanged since claim()
  // because only the consumer advances it, and we have not released yet): any
  // record at or below the in-flight write slot was (or is being) torn and must
  // be discarded.
  //
  // floor = w - numSlots + 1, NOT w - numSlots. push() writes physical slot
  // (idx % numSlots) BEFORE publishing writeIdx_ = idx+1, so when we observe
  // writeIdx_ == w the producer may be mid-overwriting the slot for index w —
  // which is the SAME physical slot that holds index (w - numSlots). We cannot
  // tell "finished w-1, idle" from "mid-writing w", so we conservatively treat
  // index (w - numSlots) as torn too. Using w - numSlots here would deliver that
  // boundary record while its slot is being overwritten — a torn quote reaching
  // a handler (silent corruption under sustained overflow). Costs at most one
  // extra (still-fresh) drop per fully-lapped batch; correct for drop-oldest.
  const uint64_t w = writeIdx_.load(std::memory_order_acquire);
  const uint64_t r = readIdx_.load(std::memory_order_relaxed);
  const uint64_t floor = (w >= numSlots_) ? (w - numSlots_ + 1) : 0;
  if (floor <= r)
    return 0; // producer never reached our batch; every decoded record is clean
  uint64_t torn = floor - r;
  if (torn > claimed)
    torn = claimed; // producer raced far past the batch; whole batch is torn
  drops_.fetch_add(torn, std::memory_order_relaxed);
  return static_cast<uint32_t>(torn);
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
