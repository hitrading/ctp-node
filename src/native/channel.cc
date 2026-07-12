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

void EventChannel::writeRecord(uint8_t *slot, int32_t eventType,
                               int32_t requestId, int32_t isLast,
                               int32_t errorId, const char *errorMsg,
                               int32_t structId, const void *payload,
                               int32_t payloadLen, bool droppable) {
  putI32(slot + SLOT_EVENT_TYPE, eventType);
  putI32(slot + SLOT_REQUEST_ID, requestId);
  putI32(slot + SLOT_IS_LAST, isLast);
  putI32(slot + SLOT_ERROR_ID, errorId);
  putI32(slot + SLOT_STRUCT_ID, structId);
  putI32(slot + SLOT_INTERNAL_FLAGS,
         droppable ? SLOT_FLAG_DROPPABLE : 0);

  if (errorMsg && errorMsg[0]) {
    size_t messageBytes = 0;
    while (messageBytes < SLOT_ERROR_MSG_CAP - 1 && errorMsg[messageBytes])
      ++messageBytes;
    std::memcpy(slot + SLOT_ERROR_MSG, errorMsg, messageBytes);
    slot[SLOT_ERROR_MSG + messageBytes] = 0;
  } else {
    slot[SLOT_ERROR_MSG] = 0;
  }

  int32_t n = 0;
  if (payload && payloadLen > 0) {
    n = std::min<int32_t>(payloadLen, static_cast<int32_t>(maxPayload_));
    std::memcpy(slot + SLOT_HEADER_SIZE, payload, static_cast<size_t>(n));
  }
  putI32(slot + SLOT_PAYLOAD_LEN, n);
}

bool EventChannel::recordDroppable(const uint8_t *slot) {
  int32_t flags = 0;
  std::memcpy(&flags, slot + SLOT_INTERNAL_FLAGS, sizeof(flags));
  return (flags & SLOT_FLAG_DROPPABLE) != 0;
}

std::vector<uint8_t> EventChannel::stageRecord(
    int32_t eventType, int32_t requestId, int32_t isLast, int32_t errorId,
    const char *errorMsg, int32_t structId, const void *payload,
    int32_t payloadLen, bool droppable) {
  const size_t payloadBytes = payload && payloadLen > 0
                                  ? std::min<size_t>(
                                        static_cast<size_t>(payloadLen),
                                        maxPayload_)
                                  : 0;
  std::vector<uint8_t> record(align8(SLOT_HEADER_SIZE + payloadBytes), 0);
  writeRecord(record.data(), eventType, requestId, isLast, errorId, errorMsg,
              structId, payload, payloadLen, droppable);
  return record;
}

void EventChannel::ringDoorbell() {
  if (!pending_.exchange(true, std::memory_order_acq_rel) && tsfn_)
    napi_call_threadsafe_function(tsfn_, nullptr, napi_tsfn_nonblocking);
}

void EventChannel::migrateUnclaimedRingLocked(uint64_t write, uint64_t read) {
  // A claimed batch belongs to JS until release() and must never be moved or
  // displaced. Move every later ring record into the stable staging list when
  // Oldest first enters overflow mode. This is what lets a tick behind an
  // interleaved control record displace an older ring tick without overwriting
  // memory JS may currently be decoding.
  const size_t physical = static_cast<size_t>(write - read);
  const size_t unclaimed = physical > claimedOldest_
                               ? physical - claimedOldest_
                               : 0;
  for (size_t i = 0; i < unclaimed; ++i) {
    const uint64_t index = read + claimedOldest_ + i;
    const uint8_t *slot = ring_.data() + (index % numSlots_) * slotSize_;
    int32_t payloadLen = 0;
    std::memcpy(&payloadLen, slot + SLOT_PAYLOAD_LEN, sizeof(payloadLen));
    const size_t payloadBytes =
        payloadLen > 0
            ? std::min<size_t>(static_cast<size_t>(payloadLen), maxPayload_)
            : 0;
    std::vector<uint8_t> record(
        slot, slot + align8(SLOT_HEADER_SIZE + payloadBytes));
    appendOverflowLocked(std::move(record), recordDroppable(slot));
  }
  skippedRing_ += unclaimed;
}

void EventChannel::appendOverflowLocked(std::vector<uint8_t> record,
                                        bool droppable) {
  overflow_.push_back(std::move(record));
  if (policy_ == DropPolicy::Oldest && droppable) {
    auto it = overflow_.end();
    --it;
    droppableOverflow_.push_back(it);
  }
}

void EventChannel::makeRoomForOldestLocked(bool incomingDroppable) {
  if (!incomingDroppable || droppableOverflow_.size() < numSlots_)
    return;

  // Control responses do not consume the bounded tick capacity. Stable list
  // iterators make this O(1), irrespective of how many controls precede the
  // oldest retained tick.
  overflow_.erase(droppableOverflow_.front());
  droppableOverflow_.pop_front();
  drops_.fetch_add(1, std::memory_order_relaxed);
}

bool EventChannel::push(int32_t eventType, int32_t requestId, int32_t isLast,
                        int32_t errorId, const char *errorMsg, int32_t structId,
                        const void *payload, int32_t payloadLen,
                        bool droppable) {
  for (;;) {
    // Once overflow starts, preserve ordering by staging every later producer
    // record until the consumer has copied the queue back into the ring.
    if (policy_ != DropPolicy::Newest &&
        overflowing_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(overflowMutex_);
      if (!overflowing_.load(std::memory_order_relaxed))
        continue; // consumer emptied it while we waited; retry the ring path
      std::vector<uint8_t> rec =
          stageRecord(eventType, requestId, isLast, errorId, errorMsg,
                      structId, payload, payloadLen, droppable);
      if (policy_ == DropPolicy::Oldest)
        makeRoomForOldestLocked(droppable);
      appendOverflowLocked(std::move(rec), droppable);
      ringDoorbell();
      return true;
    }

    const uint64_t w = writeIdx_.load(std::memory_order_relaxed);
    const uint64_t r = readIdx_.load(std::memory_order_acquire);
    if (w - r >= numSlots_) {
      if (policy_ == DropPolicy::Newest) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      // Serialize the transition to staged mode with a possible consumer refill.
      std::lock_guard<std::mutex> lk(overflowMutex_);
      if (writeIdx_.load(std::memory_order_relaxed) -
              readIdx_.load(std::memory_order_acquire) <
          numSlots_)
        continue; // consumer freed a slot before we acquired the handoff lock
      const uint64_t w2 = writeIdx_.load(std::memory_order_relaxed);
      const uint64_t r2 = readIdx_.load(std::memory_order_acquire);
      overflowing_.store(true, std::memory_order_release);
      if (policy_ == DropPolicy::Oldest)
        migrateUnclaimedRingLocked(w2, r2);
      std::vector<uint8_t> rec =
          stageRecord(eventType, requestId, isLast, errorId, errorMsg,
                      structId, payload, payloadLen, droppable);
      if (policy_ == DropPolicy::Oldest)
        makeRoomForOldestLocked(droppable);
      appendOverflowLocked(std::move(rec), droppable);
      ringDoorbell();
      return true;
    }

    uint8_t *slot = ring_.data() + (w % numSlots_) * slotSize_;
    writeRecord(slot, eventType, requestId, isLast, errorId, errorMsg,
                structId, payload, payloadLen, droppable);
    writeIdx_.store(w + 1, std::memory_order_release);
    ringDoorbell();
    return true;
  }
}

void EventChannel::refillFromOverflow() {
  if (!overflowing_.load(std::memory_order_acquire))
    return;
  std::lock_guard<std::mutex> lk(overflowMutex_);
  refillFromOverflowLocked();
}

void EventChannel::refillFromOverflowLocked() {
  if (!overflowing_.load(std::memory_order_relaxed))
    return;
  uint64_t w = writeIdx_.load(std::memory_order_relaxed);
  uint64_t r = readIdx_.load(std::memory_order_acquire);
  if (policy_ == DropPolicy::Oldest && skippedRing_ > 0) {
    const size_t skip =
        std::min(skippedRing_, static_cast<size_t>(w - r));
    r += skip;
    skippedRing_ -= skip;
    readIdx_.store(r, std::memory_order_release);
  }
  size_t free = static_cast<size_t>(numSlots_ - (w - r));
  while (free > 0 && !overflow_.empty()) {
    auto front = overflow_.begin();
    uint8_t *slot = ring_.data() + (w % numSlots_) * slotSize_;
    std::memset(slot, 0, slotSize_);
    std::memcpy(slot, front->data(), front->size());
    if (policy_ == DropPolicy::Oldest && recordDroppable(front->data())) {
      // Tick iterators follow list order, so a droppable list head is also the
      // oldest tracked tick.
      if (!droppableOverflow_.empty() && droppableOverflow_.front() == front)
        droppableOverflow_.pop_front();
    }
    overflow_.erase(front);
    ++w;
    --free;
  }
  writeIdx_.store(w, std::memory_order_release);
  if (overflow_.empty()) {
    droppableOverflow_.clear();
    overflowing_.store(false, std::memory_order_release);
  }
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
  if (policy_ == DropPolicy::Oldest) {
    std::lock_guard<std::mutex> lk(overflowMutex_);
    // The normal JS consumer always releases before claiming again. If a raw
    // caller violates that contract, do not create a second ownership window.
    if (claimedOldest_ != 0)
      return static_cast<uint32_t>(claimedOldest_);
    refillFromOverflowLocked();
    const uint64_t w = writeIdx_.load(std::memory_order_acquire);
    const uint64_t r = readIdx_.load(std::memory_order_relaxed);
    claimedOldest_ = static_cast<size_t>(w - r);
    return static_cast<uint32_t>(claimedOldest_);
  }
  refillFromOverflow();
  const uint64_t w = writeIdx_.load(std::memory_order_acquire);
  const uint64_t r = readIdx_.load(std::memory_order_relaxed);
  return static_cast<uint32_t>(w - r);
}

void EventChannel::release(uint32_t n) {
  if (policy_ == DropPolicy::Oldest) {
    std::lock_guard<std::mutex> lk(overflowMutex_);
    const uint64_t r = readIdx_.load(std::memory_order_relaxed);
    const uint64_t w = writeIdx_.load(std::memory_order_acquire);
    const uint64_t released =
        std::min<uint64_t>(std::min<uint64_t>(n, claimedOldest_), w - r);
    readIdx_.store(r + released, std::memory_order_release);
    claimedOldest_ -= static_cast<size_t>(released);
    return;
  }
  const uint64_t r = readIdx_.load(std::memory_order_relaxed);
  const uint64_t w = writeIdx_.load(std::memory_order_acquire);
  readIdx_.store(r + std::min<uint64_t>(n, w - r),
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
