/*
 * shared_handle.h - runtime-tagged N-API handle for cross-client shared state.
 *
 * Napi::External<T> does not preserve T at runtime. The handle therefore
 * carries a magic/kind tag that is verified before touching the shared_ptr, so
 * a foreign External is rejected as a recoverable JavaScript TypeError rather
 * than causing native type confusion.
 */
#ifndef CTP_NATIVE_SHARED_HANDLE_H
#define CTP_NATIVE_SHARED_HANDLE_H

#include <cstdint>
#include <memory>

namespace ctp {

class SnapshotCache;

enum class SharedHandleKind : uint32_t {
  SnapshotCache = 2,
};

struct SharedHandle {
  static constexpr uint64_t kMagic = UINT64_C(0x4354504E4F444548); // CTPNODEH

  uint64_t magic = kMagic;
  SharedHandleKind kind = SharedHandleKind::SnapshotCache;
  std::shared_ptr<SnapshotCache> snapshot;
};

} // namespace ctp

#endif /* CTP_NATIVE_SHARED_HANDLE_H */
