/*
 * channel.h - lock-free SPSC ring delivering binary CTP records from the CTP
 * callback thread (single producer) to the JS event loop (single consumer).
 *
 * The producer only memcpy's bytes + bumps an atomic index, then rings a
 * coalesced doorbell (a threadsafe-function used purely as a wakeup). The JS
 * consumer drains a whole batch per wakeup and decodes straight from the ring
 * (zero-copy) into plain objects. The CTP thread is never blocked; what happens
 * on overflow is set per channel by DropPolicy:
 *
 *   - DropPolicy::Newest  (the trader): when full the producer refuses the new
 *     record and bumps a counter. Nothing already queued is discarded, so an
 *     order/trade return is never silently lost - reliability over freshness.
 *   - DropPolicy::Oldest  (market data): when full the producer overwrites the
 *     oldest unread record so the consumer always sees the freshest quotes.
 *     The producer stays wait-free and never touches readIdx_/drops_; the
 *     consumer detects the overrun (claim() skips lapped records, validate()
 *     discards any read torn mid-overwrite) and owns the drop counter. This
 *     keeps the SPSC single-writer invariant intact: producer writes only
 *     writeIdx_, consumer writes only readIdx_ (and, in this mode, drops_).
 *
 * Slot header layout (fixed; the JS drain in client.ts must match):
 *   [0]  int32 eventType
 *   [4]  int32 requestId
 *   [8]  int32 isLast      (-1 undefined / 0 false / 1 true)
 *   [12] int32 errorId     (0 = no error)
 *   [16] int32 structId    (-1 = no payload struct)
 *   [20] int32 payloadLen
 *   [24] char  errorMsg[81]
 *   [112] payload bytes (8-aligned start)
 */

#ifndef CTP_NATIVE_CHANNEL_H
#define CTP_NATIVE_CHANNEL_H

#include <napi.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace ctp {

enum {
  SLOT_EVENT_TYPE = 0,
  SLOT_REQUEST_ID = 4,
  SLOT_IS_LAST = 8,
  SLOT_ERROR_ID = 12,
  SLOT_STRUCT_ID = 16,
  SLOT_PAYLOAD_LEN = 20,
  SLOT_ERROR_MSG = 24,
  SLOT_ERROR_MSG_CAP = 81,
  SLOT_HEADER_SIZE = 112,
};

// Overflow behaviour, fixed per channel at construction. See the file header.
enum class DropPolicy {
  Newest, // refuse the incoming record when full (reliable; trader)
  Oldest, // overwrite the oldest unread record when full (freshest; market data)
};

class EventChannel {
public:
  EventChannel(size_t numSlots, size_t maxPayload,
               DropPolicy policy = DropPolicy::Newest);
  ~EventChannel();

  // Wire the coalesced doorbell to a JS drain callback (JS thread).
  napi_status start(napi_env env, napi_value drainCb);
  // Abort the doorbell. MUST be called only after the producer has stopped.
  void stop();

  // Producer (CTP callback thread). Returns false only under DropPolicy::Newest
  // when the ring is full (the record was refused + counted); DropPolicy::Oldest
  // always returns true (it overwrites the oldest unread record).
  bool push(int32_t eventType, int32_t requestId, int32_t isLast,
            int32_t errorId, const char *errorMsg, int32_t structId,
            const void *payload, int32_t payloadLen);

  // Consumer (JS thread).
  uint32_t claim();          // clear pending, return available record count
  void release(uint32_t n);  // free n drained slots
  // DropPolicy::Oldest only: after claim() returned `claimed` and the consumer
  // decoded that many records from the current readIdx_, report how many of the
  // OLDEST decoded records the producer overwrote *during* the decode (it lapped
  // the ring). Those records hold torn bytes and must be discarded; they are
  // added to the drop counter. Always 0 under DropPolicy::Newest (the producer
  // never overwrites unread slots).
  uint32_t validate(uint32_t claimed);

  uint64_t dropCount() const { return drops_.load(std::memory_order_relaxed); }
  uint64_t readIndex() const { return readIdx_.load(std::memory_order_relaxed); }
  bool dropOldest() const { return policy_ == DropPolicy::Oldest; }
  size_t numSlots() const { return numSlots_; }
  size_t slotSize() const { return slotSize_; }
  size_t headerSize() const { return SLOT_HEADER_SIZE; }
  uint8_t *data() { return ring_.data(); }
  size_t byteLength() const { return ring_.size(); }

private:
  static void onDoorbell(napi_env env, napi_value jsCb, void *ctx, void *data);

  size_t numSlots_;
  size_t slotSize_;
  size_t maxPayload_;
  DropPolicy policy_;
  std::vector<uint8_t> ring_;
  std::atomic<uint64_t> writeIdx_{0};
  std::atomic<uint64_t> readIdx_{0};
  std::atomic<uint64_t> drops_{0};
  std::atomic<bool> pending_{false};
  napi_threadsafe_function tsfn_{nullptr};
};

} // namespace ctp

#endif /* CTP_NATIVE_CHANNEL_H */
