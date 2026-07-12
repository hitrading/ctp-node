/*
 * channel.h - lock-free SPSC ring delivering binary CTP records from the CTP
 * callback thread (single producer) to the JS event loop (single consumer).
 *
 * The producer only memcpy's bytes + bumps an atomic index, then rings a
 * coalesced doorbell (a threadsafe-function used purely as a wakeup). The JS
 * consumer drains a whole batch per wakeup and decodes straight from the ring
 * (zero-copy) into plain objects. The fixed-ring fast path is lock-free; the
 * overload staging path may briefly take a mutex. Overflow behavior is set per
 * channel by DropPolicy:
 *
 *   - DropPolicy::Newest: when full the producer refuses the incoming record.
 *     Retained for small self-tests; production clients do not use it.
 *   - DropPolicy::Oldest (market data): droppable tick overflow enters a bounded
 *     staging queue and displaces the oldest unread droppable tick. Control
 *     responses are never deliberately discarded; control records are excluded
 *     from the bounded keep-newest tick capacity. The
 *     ring is never overwritten while JS reads it.
 *   - DropPolicy::Lossless (trader): overflow records enter an unbounded staging
 *     queue and are copied back into the ring in order as the consumer frees
 *     slots. The allocation/lock path is reached only during overload; normal
 *     callbacks retain the zero-copy SPSC fast path. Critical order/trade/query
 *     responses are therefore never deliberately discarded.
 *
 * Slot header layout (fixed; the JS drain in client.ts must match):
 *   [0]  int32 eventType
 *   [4]  int32 requestId
 *   [8]  int32 isLast      (-1 undefined / 0 false / 1 true)
 *   [12] int32 errorId     (0 = no error)
 *   [16] int32 structId    (-1 = no payload struct)
 *   [20] int32 payloadLen
 *   [24] char  errorMsg[81]
 *   [108]int32 internal flags (not exposed to JS)
 *   [112] payload bytes (8-aligned start)
 */

#ifndef CTP_NATIVE_CHANNEL_H
#define CTP_NATIVE_CHANNEL_H

#include <napi.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
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
  SLOT_INTERNAL_FLAGS = 108,
  SLOT_HEADER_SIZE = 112,
};

enum { SLOT_FLAG_DROPPABLE = 1 };

// Overflow behaviour, fixed per channel at construction. See the file header.
enum class DropPolicy {
  Newest,   // refuse incoming record when full (test-only)
  Oldest,   // bounded staged overflow, discard oldest staged record
  Lossless, // unbounded staged overflow, preserve every record (trader)
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
  // when the ring is full. Oldest/Lossless never touch slots the JS consumer may
  // currently be reading. Under Oldest, only records explicitly marked
  // `droppable` may be discarded; MD control responses use the default false.
  bool push(int32_t eventType, int32_t requestId, int32_t isLast,
            int32_t errorId, const char *errorMsg, int32_t structId,
            const void *payload, int32_t payloadLen,
            bool droppable = false);

  // Consumer (JS thread).
  uint32_t claim();          // clear pending, return available record count
  void release(uint32_t n);  // free n drained slots

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
  void writeRecord(uint8_t *slot, int32_t eventType, int32_t requestId,
                   int32_t isLast, int32_t errorId, const char *errorMsg,
                   int32_t structId, const void *payload, int32_t payloadLen,
                   bool droppable);
  static bool recordDroppable(const uint8_t *slot);
  std::vector<uint8_t> stageRecord(int32_t eventType, int32_t requestId,
                                   int32_t isLast, int32_t errorId,
                                   const char *errorMsg, int32_t structId,
                                   const void *payload, int32_t payloadLen,
                                   bool droppable);
  void ringDoorbell();
  using StagedRecords = std::list<std::vector<uint8_t>>;
  using StagedRecordIterator = StagedRecords::iterator;
  void migrateUnclaimedRingLocked(uint64_t write, uint64_t read);
  void appendOverflowLocked(std::vector<uint8_t> record, bool droppable);
  void makeRoomForOldestLocked(bool incomingDroppable);
  void refillFromOverflow(); // consumer thread
  void refillFromOverflowLocked(); // overflowMutex_ held

  size_t numSlots_;
  size_t slotSize_;
  size_t maxPayload_;
  DropPolicy policy_;
  std::vector<uint8_t> ring_;
  std::atomic<uint64_t> writeIdx_{0};
  std::atomic<uint64_t> readIdx_{0};
  std::atomic<uint64_t> drops_{0};
  std::atomic<bool> pending_{false};
  // Slow-path handoff used only after the fixed ring fills. While overflowing_
  // is true the producer writes exclusively to overflow_, allowing the consumer
  // (under overflowMutex_) to become the temporary ring writer while refilling.
  std::atomic<bool> overflowing_{false};
  std::mutex overflowMutex_;
  // A list gives each staged record a stable iterator. Oldest-policy tick
  // iterators are also kept in FIFO order below, so displacing the oldest tick
  // remains O(1) even when non-droppable control records are interleaved.
  StagedRecords overflow_;
  std::deque<StagedRecordIterator> droppableOverflow_;
  // Number of oldest logical ring records displaced by staged Oldest records.
  // Guarded by overflowMutex_; refill advances over them before the next claim.
  size_t skippedRing_ = 0;
  // Oldest-policy records returned by claim() are owned by JS until release().
  // They may no longer be logically displaced while the decoded batch is being
  // delivered. Guarded by overflowMutex_.
  size_t claimedOldest_ = 0;
  napi_threadsafe_function tsfn_{nullptr};
};

} // namespace ctp

#endif /* CTP_NATIVE_CHANNEL_H */
