/*
 * shared_handle.h - runtime-tagged N-API handles for cross-client shared state.
 *
 * Napi::External<T> does not preserve T at runtime.  Both sides therefore use
 * this one concrete payload and verify its magic/kind before touching either
 * shared_ptr.  This prevents an arm-registry External from being interpreted as
 * a SnapshotCache External (or vice versa), which would otherwise be native
 * type confusion rather than a recoverable JavaScript TypeError.
 */
#ifndef CTP_NATIVE_SHARED_HANDLE_H
#define CTP_NATIVE_SHARED_HANDLE_H

#include <cstdint>
#include <memory>

namespace ctp {

class ArmRegistry;
class SnapshotCache;

enum class SharedHandleKind : uint32_t {
  ArmRegistry = 1,
  SnapshotCache = 2,
};

struct SharedHandle {
  static constexpr uint64_t kMagic = UINT64_C(0x4354504E4F444548); // CTPNODEH

  uint64_t magic = kMagic;
  SharedHandleKind kind = SharedHandleKind::ArmRegistry;
  std::shared_ptr<ArmRegistry> arm;
  std::shared_ptr<SnapshotCache> snapshot;
};

} // namespace ctp

#endif /* CTP_NATIVE_SHARED_HANDLE_H */
