// Shared LV2 kernel object infrastructure.
#pragma once
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "runtime/hle.h"
#include "runtime/ppu_thread.h"

namespace rt::lv2 {

// Process identity: SDK version from the executable's process parameters; program authority ID
// and control flags from the game's SELF header (plaintext fields of the user's EBOOT.BIN).
extern u32 g_sdk_version;
extern u64 g_auth_id;
extern u32 g_ctrl_flags1;
bool read_self_identity(const std::string& self_path);

template <typename T>
class IdMap {
 public:
  explicit IdMap(u32 base) : next_(base) {}
  u32 add(std::shared_ptr<T> p) {
    std::lock_guard lk(mu_);
    u32 id = next_++;
    m_[id] = std::move(p);
    return id;
  }
  std::shared_ptr<T> get(u32 id) {
    std::lock_guard lk(mu_);
    auto it = m_.find(id);
    return it == m_.end() ? nullptr : it->second;
  }
  std::shared_ptr<T> remove(u32 id) {
    std::lock_guard lk(mu_);
    auto it = m_.find(id);
    if (it == m_.end()) return nullptr;
    auto p = it->second;
    m_.erase(it);
    return p;
  }
  template <typename F>
  void for_each(F f) {
    std::lock_guard lk(mu_);
    for (auto& [id, p] : m_) f(id, *p);
  }

 private:
  std::mutex mu_;
  std::map<u32, std::shared_ptr<T>> m_;
  u32 next_;
};

inline u32 self_id() {
  PpuThread* t = cur_thread();
  return t ? t->id : 0xffffffffu;
}

// Waits on cv until pred() with an LV2 timeout in microseconds (0 = forever).
// Returns false on timeout (pred still false).
template <typename Lock, typename Pred>
bool wait_us(std::condition_variable& cv, Lock& lk, u64 timeout_us, Pred pred) {
  if (pred()) return true;
  note_block();
  if (!timeout_us) {
    cv.wait(lk, pred);
    return true;
  }
  return cv.wait_for(lk, std::chrono::microseconds(timeout_us), pred);
}

// Mutex ownership with LV2 hand-off semantics. With SYS_SYNC_FIFO (1) or SYS_SYNC_PRIORITY (2,
// and 3 = priority inherit) an unlock passes ownership directly to the next waiter (oldest, or
// highest priority first), so a thread that unlocks and immediately relocks cannot starve the
// waiters. SYS_SYNC_RETRY (4) wakes a waiter that then competes for the lock.
struct FairOwner {
  u32 owner = 0, count = 0;
  u32 protocol = 1;
  std::vector<u32> queue;  // waiting thread ids in arrival order
};
template <typename Lock>
bool fair_acquire(FairOwner& o, std::condition_variable& cv, Lock& lk, u32 me, u64 timeout_us) {
  if (o.owner == 0 && (o.queue.empty() || o.protocol == 4)) {
    o.owner = me;
    return true;
  }
  o.queue.push_back(me);
  bool ok = wait_us(cv, lk, timeout_us, [&] { return o.owner == me || (o.protocol == 4 && o.owner == 0); });
  o.queue.erase(std::find(o.queue.begin(), o.queue.end(), me));
  if (ok && o.owner == 0) o.owner = me;  // retry protocol
  return ok;
}
// Releases ownership; returns true if waiters must be notified.
inline bool fair_release(FairOwner& o) {
  o.owner = 0;
  if (o.queue.empty()) return false;
  if (o.protocol == 4) return true;
  size_t pick = 0;
  if (o.protocol == 2 || o.protocol == 3) {
    s32 best = 0x7fffffff;
    for (size_t i = 0; i < o.queue.size(); ++i) {
      PpuThread* t = thread_get(o.queue[i]);
      s32 p = t ? t->prio : 0x7fffffff;
      if (p < best) {
        best = p;
        pick = i;
      }
    }
  }
  o.owner = o.queue[pick];
  return true;
}

u64 system_time_us();  // microseconds since process start (LV2 system time)
std::string name8(const void* p);  // 8-byte object names

}  // namespace rt::lv2
