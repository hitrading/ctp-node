/*
 * snapshot.h - last-value cache (LVC): the latest full depth tick per instrument.
 *
 * Written on the CTP MD callback thread (update), read on the JS thread
 * (get/last) and the Trader's order thread (last, via RefPriceSource, for the
 * deviation reference). One mutex; entries are full DepthMarketDataField copies.
 * Cleared on unsubscribe (erase) and close (clear). This is the in-C++ market-
 * data cache, so latest prices reach the risk engine without round-tripping
 * through JS, and JS can read a synchronous snapshot without waiting for a tick.
 */
#ifndef CTP_NATIVE_SNAPSHOT_H
#define CTP_NATIVE_SNAPSHOT_H

#include "ThostFtdcUserApiStruct.h"
#include "risk.h" // RefPriceSource
#include <mutex>
#include <string>
#include <unordered_map>

namespace ctp {

class SnapshotCache : public RefPriceSource {
public:
  // MD callback thread: copy the whole latest tick in, keyed by instrument id.
  void update(const CThostFtdcDepthMarketDataField &t) {
    std::lock_guard<std::mutex> lk(m_);
    map_[t.InstrumentID] = t;
  }
  // Reader (JS thread): copy the latest tick out. False if none cached.
  bool get(const std::string &id, CThostFtdcDepthMarketDataField &out) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = map_.find(id);
    if (it == map_.end())
      return false;
    out = it->second;
    return true;
  }
  // RefPriceSource: latest price, or 0 if absent. Validity (the DBL_MAX "unset"
  // sentinel etc.) is filtered by the risk engine's refPrice(), not here.
  double last(const std::string &id) const override {
    std::lock_guard<std::mutex> lk(m_);
    auto it = map_.find(id);
    return it != map_.end() ? it->second.LastPrice : 0.0;
  }
  void erase(const std::string &id) { // unsubscribe
    std::lock_guard<std::mutex> lk(m_);
    map_.erase(id);
  }
  void clear() { // close
    std::lock_guard<std::mutex> lk(m_);
    map_.clear();
  }

private:
  mutable std::mutex m_;
  std::unordered_map<std::string, CThostFtdcDepthMarketDataField> map_;
};

} // namespace ctp

#endif /* CTP_NATIVE_SNAPSHOT_H */
