/*
 * snapshot.h - last-value cache (LVC): the latest full depth tick per instrument.
 *
 * Written on the CTP MD callback thread (update), read on the JS thread
 * (get/last) and the Trader's order thread (quote(), via RefPriceSource, for the
 * deviation reference). One mutex; entries are full DepthMarketDataField copies.
 * A cache is invalid until the market-data login succeeds. It is invalidated
 * (and cleared) on every front/session transition, so a reconnect can never
 * make a quote from the previous session look fresh merely because its local
 * steady-clock age is small. This is the in-C++ market-data cache, so latest
 * prices reach the risk engine without round-tripping through JS, and JS can
 * read a synchronous snapshot without waiting for a tick.
 */
#ifndef CTP_NATIVE_SNAPSHOT_H
#define CTP_NATIVE_SNAPSHOT_H

#include "ThostFtdcUserApiStruct.h"
#include "risk.h" // RefPriceSource
#include <cmath>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ctp {

class SnapshotCache : public RefPriceSource {
public:
  // Start a newly authenticated MD session. Always discard prior entries before
  // making the cache readable: activation itself must not resurrect stale quotes.
  void activate() {
    std::lock_guard<std::mutex> lk(m_);
    map_.clear();
    active_ = true;
  }

  // Close the current MD session immediately. update() also checks active_, so a
  // callback racing just after disconnect/logout cannot repopulate the cache.
  void invalidate() {
    std::lock_guard<std::mutex> lk(m_);
    active_ = false;
    map_.clear();
  }

  // MD callback thread: copy the whole latest tick in, keyed by instrument id.
  bool update(const std::string &instrumentId,
              const CThostFtdcDepthMarketDataField &t) {
    std::lock_guard<std::mutex> lk(m_);
    if (!active_ || instrumentId.empty())
      return false;
    map_[instrumentId] = Entry{t, std::chrono::steady_clock::now()};
    return true;
  }
  // Reader (JS thread): copy the latest tick out. False if none cached.
  bool get(const std::string &id, CThostFtdcDepthMarketDataField &out) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!active_)
      return false;
    auto it = map_.find(id);
    if (it == map_.end())
      return false;
    out = it->second.tick;
    return true;
  }
  // JS convenience: latest usable price, or 0 if absent/unset.
  double last(const std::string &id) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!active_)
      return 0.0;
    auto it = map_.find(id);
    if (it == map_.end())
      return 0.0;
    const double price = it->second.tick.LastPrice;
    return std::isfinite(price) && price > 0.0 && price < 1e300 ? price : 0.0;
  }
  uint64_t ageMillis(const std::string &id) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!active_)
      return std::numeric_limits<uint64_t>::max();
    auto it = map_.find(id);
    if (it == map_.end())
      return std::numeric_limits<uint64_t>::max();
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - it->second.updated);
    return age.count() < 0 ? 0 : static_cast<uint64_t>(age.count());
  }
  bool quote(const std::string &id, double &price,
             uint64_t &ageMillisOut) const override {
    std::lock_guard<std::mutex> lk(m_);
    if (!active_)
      return false;
    auto it = map_.find(id);
    if (it == map_.end())
      return false;
    price = it->second.tick.LastPrice;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - it->second.updated);
    ageMillisOut = age.count() < 0 ? 0 : static_cast<uint64_t>(age.count());
    return true;
  }
  void erase(const std::string &id) { // unsubscribe
    std::lock_guard<std::mutex> lk(m_);
    map_.erase(id);
  }
  void clear() { invalidate(); } // close

private:
  struct Entry {
    CThostFtdcDepthMarketDataField tick;
    std::chrono::steady_clock::time_point updated;
  };
  mutable std::mutex m_;
  bool active_ = false;
  std::unordered_map<std::string, Entry> map_;
};

} // namespace ctp

#endif /* CTP_NATIVE_SNAPSHOT_H */
