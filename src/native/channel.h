/*
 * channel.h - lock-free SPSC ring delivering binary CTP records from the CTP
 * callback thread (single producer) to the JS event loop (single consumer).
 *
 * The producer only memcpy's bytes + bumps an atomic index, then rings a
 * coalesced doorbell (a threadsafe-function used purely as a wakeup). The JS
 * consumer drains a whole batch per wakeup and decodes straight from the ring
 * (zero-copy) into plain objects. Backpressure: drop-newest + a counter when
 * full, so the CTP thread is never blocked.
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

class EventChannel {
public:
  EventChannel(size_t numSlots, size_t maxPayload);
  ~EventChannel();

  // Wire the coalesced doorbell to a JS drain callback (JS thread).
  napi_status start(napi_env env, napi_value drainCb);
  // Abort the doorbell. MUST be called only after the producer has stopped.
  void stop();

  // Producer (CTP callback thread). false (+drop) if the ring is full.
  bool push(int32_t eventType, int32_t requestId, int32_t isLast,
            int32_t errorId, const char *errorMsg, int32_t structId,
            const void *payload, int32_t payloadLen);

  // Consumer (JS thread).
  uint32_t claim();          // clear pending, return available record count
  void release(uint32_t n);  // free n drained slots

  uint64_t dropCount() const { return drops_.load(std::memory_order_relaxed); }
  uint64_t readIndex() const { return readIdx_.load(std::memory_order_relaxed); }
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
  std::vector<uint8_t> ring_;
  std::atomic<uint64_t> writeIdx_{0};
  std::atomic<uint64_t> readIdx_{0};
  std::atomic<uint64_t> drops_{0};
  std::atomic<bool> pending_{false};
  napi_threadsafe_function tsfn_{nullptr};
};

} // namespace ctp

#endif /* CTP_NATIVE_CHANNEL_H */
