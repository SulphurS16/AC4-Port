// LV2 synchronization objects: mutex, cond, semaphore, rwlock, event queue/port.
// Semantics follow the LV2 syscall interface (error codes, recursion, timeouts in microseconds).
#include <algorithm>
#include <deque>
#include <set>

#include "runtime/lv2/lv2.h"
#include "runtime/lv2/sync.h"

namespace rt::lv2 {

std::string name8(const void* p) {
  const char* c = static_cast<const char*>(p);
  size_t n = strnlen(c, 8);
  return std::string(c, n);
}

namespace {

constexpr u32 SYS_SYNC_RECURSIVE = 0x10;

struct AttrMutex {
  be32 protocol, recursive, pshared, adaptive;
  be64 ipc_key;
  bes32 flags;
  be32 pad;
  char name[8];
};
struct AttrCond {
  be32 pshared;
  bes32 flags;
  be64 ipc_key;
  char name[8];
};
struct AttrSema {
  be32 protocol, pshared;
  be64 ipc_key;
  bes32 flags;
  be32 pad;
  char name[8];
};
using AttrRwlock = AttrSema;
struct AttrQueue {
  be32 protocol;
  bes32 type;
  char name[8];
};

// ---- mutex ----
struct Mutex {
  std::mutex m;
  std::condition_variable cv;
  FairOwner o;
  u32 conds = 0;
  bool recursive = false;
  std::string name;
};
IdMap<Mutex> g_mutex(0x85000000);

s32 lock_mutex(Mutex& mx, std::unique_lock<std::mutex>& lk, u32 me, u64 timeout) {
  if (mx.o.owner == me) {
    if (!mx.recursive) return CELL_EDEADLK;
    if (mx.o.count == 0xffffffffu) return CELL_EKRESOURCE;
    ++mx.o.count;
    return CELL_OK;
  }
  if (!fair_acquire(mx.o, mx.cv, lk, me, timeout)) return CELL_ETIMEDOUT;
  mx.o.count = 1;
  return CELL_OK;
}
// Fully releases the mutex (any recursion count) and wakes the next owner.
void release_mutex(Mutex& mx) {
  mx.o.count = 0;
  if (fair_release(mx.o)) mx.cv.notify_all();
}

s32 sys_mutex_create(Ptr<be32> id, Ptr<AttrMutex> attr) {
  if (!id || !attr) return CELL_EFAULT;
  auto mx = std::make_shared<Mutex>();
  mx->recursive = (attr->recursive & 0xf0) == SYS_SYNC_RECURSIVE;
  mx->o.protocol = attr->protocol & 0xf;
  mx->name = name8(attr->name);
  *id = g_mutex.add(mx);
  LOG_D("lv2", "sys_mutex_create '%s' -> 0x%x (recursive %d)", mx->name.c_str(), u32(*id), mx->recursive);
  return CELL_OK;
}
s32 sys_mutex_destroy(u32 id) {
  auto mx = g_mutex.get(id);
  if (!mx) return CELL_ESRCH;
  {
    std::lock_guard lk(mx->m);
    if (mx->o.owner) return CELL_EBUSY;
    if (mx->conds) return CELL_EPERM;
  }
  g_mutex.remove(id);
  return CELL_OK;
}
s32 sys_mutex_lock(u32 id, u64 timeout) {
  auto mx = g_mutex.get(id);
  if (!mx) return CELL_ESRCH;
  std::unique_lock lk(mx->m);
  return lock_mutex(*mx, lk, self_id(), timeout);
}
s32 sys_mutex_trylock(u32 id) {
  auto mx = g_mutex.get(id);
  if (!mx) return CELL_ESRCH;
  std::unique_lock lk(mx->m);
  u32 me = self_id();
  if (mx->o.owner != me && (mx->o.owner || !mx->o.queue.empty())) return CELL_EBUSY;
  return lock_mutex(*mx, lk, me, 0);
}
s32 sys_mutex_unlock(u32 id) {
  auto mx = g_mutex.get(id);
  if (!mx) return CELL_ESRCH;
  std::unique_lock lk(mx->m);
  if (mx->o.owner != self_id()) return CELL_EPERM;
  if (--mx->o.count == 0) release_mutex(*mx);
  return CELL_OK;
}

// ---- cond ----
// A signal selects one queued waiter (by the mutex's protocol) and wakes exactly that thread; a
// thread that starts waiting afterwards cannot take it. Signals with no waiter are lost.
struct Cond {
  std::shared_ptr<Mutex> mx;
  std::condition_variable cv;
  std::vector<u32> queue;  // waiting thread ids in arrival order
  std::set<u32> woken;     // selected by a signal, not yet returned from the wait
  std::string name;
  u32 waiters() const { return u32(queue.size() + woken.size()); }
};
// Removes and returns the waiter a signal wakes: highest priority for the priority protocols,
// otherwise the longest waiting.
u32 pick_waiter(std::vector<u32>& q, u32 protocol) {
  size_t pick = 0;
  if (protocol == 2 || protocol == 3) {
    s32 best = 0x7fffffff;
    for (size_t i = 0; i < q.size(); ++i) {
      PpuThread* t = thread_get(q[i]);
      s32 p = t ? t->prio : 0x7fffffff;
      if (p < best) {
        best = p;
        pick = i;
      }
    }
  }
  u32 id = q[pick];
  q.erase(q.begin() + std::ptrdiff_t(pick));
  return id;
}
IdMap<Cond> g_cond(0x86000000);

s32 sys_cond_create(Ptr<be32> id, u32 mutex_id, Ptr<AttrCond> attr) {
  auto mx = g_mutex.get(mutex_id);
  if (!mx) return CELL_ESRCH;
  if (!id || !attr) return CELL_EFAULT;
  auto c = std::make_shared<Cond>();
  c->mx = mx;
  c->name = name8(attr->name);
  {
    std::lock_guard lk(mx->m);
    ++mx->conds;
  }
  *id = g_cond.add(c);
  return CELL_OK;
}
s32 sys_cond_destroy(u32 id) {
  auto c = g_cond.get(id);
  if (!c) return CELL_ESRCH;
  {
    std::lock_guard lk(c->mx->m);
    if (c->waiters()) return CELL_EBUSY;
    --c->mx->conds;
  }
  g_cond.remove(id);
  return CELL_OK;
}
s32 sys_cond_wait(u32 id, u64 timeout) {
  auto c = g_cond.get(id);
  if (!c) return CELL_ESRCH;
  Mutex& mx = *c->mx;
  std::unique_lock lk(mx.m);
  u32 me = self_id();
  if (mx.o.owner != me) return CELL_EPERM;
  u32 saved = mx.o.count;
  release_mutex(mx);
  c->queue.push_back(me);
  bool ok = wait_us(c->cv, lk, timeout, [&] { return c->woken.count(me) != 0; });
  if (!c->woken.erase(me)) c->queue.erase(std::find(c->queue.begin(), c->queue.end(), me));  // timed out
  // Reacquire the mutex (timeouts still reacquire before returning).
  fair_acquire(mx.o, mx.cv, lk, me, 0);
  mx.o.count = saved;
  return ok ? CELL_OK : CELL_ETIMEDOUT;
}
s32 sys_cond_signal(u32 id) {
  auto c = g_cond.get(id);
  if (!c) return CELL_ESRCH;
  std::lock_guard lk(c->mx->m);
  if (!c->queue.empty()) {
    c->woken.insert(pick_waiter(c->queue, c->mx->o.protocol));
    c->cv.notify_all();
  }
  return CELL_OK;
}
s32 sys_cond_signal_all(u32 id) {
  auto c = g_cond.get(id);
  if (!c) return CELL_ESRCH;
  std::lock_guard lk(c->mx->m);
  c->woken.insert(c->queue.begin(), c->queue.end());
  c->queue.clear();
  c->cv.notify_all();
  return CELL_OK;
}
s32 sys_cond_signal_to(u32 id, u32 thread_id) {
  auto c = g_cond.get(id);
  if (!c) return CELL_ESRCH;
  std::lock_guard lk(c->mx->m);
  auto it = std::find(c->queue.begin(), c->queue.end(), thread_id);
  if (it == c->queue.end()) return CELL_EPERM;
  c->queue.erase(it);
  c->woken.insert(thread_id);
  c->cv.notify_all();
  return CELL_OK;
}

// ---- semaphore ----
struct Sema {
  std::mutex m;
  std::condition_variable cv;
  s32 val = 0, max = 0;
  u32 waiters = 0;
  std::string name;
};
IdMap<Sema> g_sema(0x96000000);

s32 sys_semaphore_create(Ptr<be32> id, Ptr<AttrSema> attr, s32 initial, s32 max) {
  if (max <= 0 || initial < 0 || initial > max) return CELL_EINVAL;
  if (!id || !attr) return CELL_EFAULT;
  auto s = std::make_shared<Sema>();
  s->val = initial;
  s->max = max;
  s->name = name8(attr->name);
  *id = g_sema.add(s);
  return CELL_OK;
}
s32 sys_semaphore_destroy(u32 id) {
  auto s = g_sema.get(id);
  if (!s) return CELL_ESRCH;
  {
    std::lock_guard lk(s->m);
    if (s->waiters) return CELL_EBUSY;
  }
  g_sema.remove(id);
  return CELL_OK;
}
s32 sys_semaphore_wait(u32 id, u64 timeout) {
  auto s = g_sema.get(id);
  if (!s) return CELL_ESRCH;
  std::unique_lock lk(s->m);
  ++s->waiters;
  bool ok = wait_us(s->cv, lk, timeout, [&] { return s->val > 0; });
  --s->waiters;
  if (!ok) return CELL_ETIMEDOUT;
  --s->val;
  return CELL_OK;
}
s32 sys_semaphore_trywait(u32 id) {
  auto s = g_sema.get(id);
  if (!s) return CELL_ESRCH;
  std::lock_guard lk(s->m);
  if (s->val <= 0) return CELL_EBUSY;
  --s->val;
  return CELL_OK;
}
s32 sys_semaphore_post(u32 id, s32 count) {
  if (count < 0) return CELL_EINVAL;
  auto s = g_sema.get(id);
  if (!s) return CELL_ESRCH;
  {
    std::lock_guard lk(s->m);
    if (s64(s->val) + count > s->max) return CELL_EBUSY;
    s->val += count;
  }
  s->cv.notify_all();
  return CELL_OK;
}
s32 sys_semaphore_get_value(u32 id, Ptr<bes32> out) {
  auto s = g_sema.get(id);
  if (!s) return CELL_ESRCH;
  if (!out) return CELL_EFAULT;
  std::lock_guard lk(s->m);
  *out = s->val;
  return CELL_OK;
}

// ---- rwlock (writer preference) ----
struct Rwlock {
  std::mutex m;
  std::condition_variable cv;
  u32 readers = 0, writer = 0, wwait = 0;
  std::string name;
};
IdMap<Rwlock> g_rwlock(0x88000000);

s32 sys_rwlock_create(Ptr<be32> id, Ptr<AttrRwlock> attr) {
  if (!id || !attr) return CELL_EFAULT;
  auto r = std::make_shared<Rwlock>();
  r->name = name8(attr->name);
  *id = g_rwlock.add(r);
  return CELL_OK;
}
s32 sys_rwlock_destroy(u32 id) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  {
    std::lock_guard lk(r->m);
    if (r->readers || r->writer) return CELL_EBUSY;
  }
  g_rwlock.remove(id);
  return CELL_OK;
}
s32 sys_rwlock_rlock(u32 id, u64 timeout) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  std::unique_lock lk(r->m);
  if (r->writer == self_id()) return CELL_EDEADLK;
  if (!wait_us(r->cv, lk, timeout, [&] { return !r->writer && !r->wwait; })) return CELL_ETIMEDOUT;
  ++r->readers;
  return CELL_OK;
}
s32 sys_rwlock_tryrlock(u32 id) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  std::lock_guard lk(r->m);
  if (r->writer || r->wwait) return CELL_EBUSY;
  ++r->readers;
  return CELL_OK;
}
s32 sys_rwlock_runlock(u32 id) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  std::unique_lock lk(r->m);
  if (!r->readers) return CELL_EPERM;
  if (--r->readers == 0) {
    lk.unlock();
    r->cv.notify_all();
  }
  return CELL_OK;
}
s32 sys_rwlock_wlock(u32 id, u64 timeout) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  std::unique_lock lk(r->m);
  u32 me = self_id();
  if (r->writer == me) return CELL_EDEADLK;
  ++r->wwait;
  bool ok = wait_us(r->cv, lk, timeout, [&] { return !r->writer && !r->readers; });
  --r->wwait;
  if (!ok) {
    r->cv.notify_all();
    return CELL_ETIMEDOUT;
  }
  r->writer = me;
  return CELL_OK;
}
s32 sys_rwlock_trywlock(u32 id) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  std::lock_guard lk(r->m);
  if (r->writer == self_id()) return CELL_EDEADLK;
  if (r->writer || r->readers) return CELL_EBUSY;
  r->writer = self_id();
  return CELL_OK;
}
s32 sys_rwlock_wunlock(u32 id) {
  auto r = g_rwlock.get(id);
  if (!r) return CELL_ESRCH;
  {
    std::lock_guard lk(r->m);
    if (r->writer != self_id()) return CELL_EPERM;
    r->writer = 0;
  }
  r->cv.notify_all();
  return CELL_OK;
}

// ---- event queue / port ----
struct Event {
  u64 source, d1, d2, d3;
};
struct Queue {
  std::mutex m;
  std::condition_variable cv;
  std::deque<Event> events;
  s32 size = 0, type = 1;
  u64 key = 0;
  u32 waiters = 0;
  bool destroyed = false;
  std::string name;
};
IdMap<Queue> g_queue(0x8d000000);

struct Port {
  std::mutex m;
  s32 type = 1;
  u64 name = 0;
  u32 queue = 0;
};
IdMap<Port> g_port(0x0e000000);

s32 sys_event_queue_create(Ptr<be32> id, Ptr<AttrQueue> attr, u64 key, s32 size) {
  if (size <= 0 || size > 127) return CELL_EINVAL;
  if (!id || !attr) return CELL_EFAULT;
  auto q = std::make_shared<Queue>();
  q->size = size;
  q->type = attr->type;
  q->key = key;
  q->name = name8(attr->name);
  *id = g_queue.add(q);
  LOG_D("lv2", "sys_event_queue_create '%s' size %d type %d -> 0x%x", q->name.c_str(), size, q->type, u32(*id));
  return CELL_OK;
}
s32 sys_event_queue_destroy(u32 id, s32 mode) {
  auto q = g_queue.get(id);
  if (!q) return CELL_ESRCH;
  {
    std::lock_guard lk(q->m);
    if (q->waiters && mode != 1 /*SYS_EVENT_QUEUE_DESTROY_FORCE*/) return CELL_EBUSY;
    q->destroyed = true;
  }
  q->cv.notify_all();
  g_queue.remove(id);
  return CELL_OK;
}
s32 sys_event_queue_receive(ppu::Ctx& ctx, u32 id, u32 dummy, u64 timeout) {
  auto q = g_queue.get(id);
  if (!q) return CELL_ESRCH;
  if (q->type != 1) return CELL_EINVAL;
  std::unique_lock lk(q->m);
  ++q->waiters;
  bool ok = wait_us(q->cv, lk, timeout, [&] { return !q->events.empty() || q->destroyed; });
  --q->waiters;
  if (q->destroyed) return CELL_ECANCELED;
  if (!ok) return CELL_ETIMEDOUT;
  Event e = q->events.front();
  q->events.pop_front();
  LOG_D("lv2", "event queue 0x%x '%s': received (0x%llx, 0x%llx, 0x%llx, 0x%llx), %zu left", id, q->name.c_str(),
        (unsigned long long)e.source, (unsigned long long)e.d1, (unsigned long long)e.d2, (unsigned long long)e.d3, q->events.size());
  ctx.r[4] = e.source;
  ctx.r[5] = e.d1;
  ctx.r[6] = e.d2;
  ctx.r[7] = e.d3;
  return CELL_OK;
}
s32 sys_event_queue_tryreceive(u32 id, u32 arr, s32 size, Ptr<be32> number) {
  auto q = g_queue.get(id);
  if (!q) return CELL_ESRCH;
  if (size < 0) return CELL_EINVAL;
  std::lock_guard lk(q->m);
  s32 n = 0;
  while (n < size && !q->events.empty()) {
    Event e = q->events.front();
    q->events.pop_front();
    u32 a = arr + 32 * u32(n++);
    ppu::w64(a, e.source);
    ppu::w64(a + 8, e.d1);
    ppu::w64(a + 16, e.d2);
    ppu::w64(a + 24, e.d3);
  }
  *number = u32(n);
  return CELL_OK;
}
s32 sys_event_queue_drain(u32 id) {
  auto q = g_queue.get(id);
  if (!q) return CELL_ESRCH;
  std::lock_guard lk(q->m);
  q->events.clear();
  return CELL_OK;
}
s32 sys_event_port_create(Ptr<be32> id, s32 type, u64 name) {
  if (type != 1 && type != 3) return CELL_EINVAL;
  if (!id) return CELL_EFAULT;
  auto p = std::make_shared<Port>();
  p->type = type;
  p->name = name;
  *id = g_port.add(p);
  return CELL_OK;
}
s32 sys_event_port_destroy(u32 id) {
  auto p = g_port.get(id);
  if (!p) return CELL_ESRCH;
  {
    std::lock_guard lk(p->m);
    if (p->queue) return CELL_EISCONN;
  }
  g_port.remove(id);
  return CELL_OK;
}
s32 sys_event_port_connect_local(u32 port, u32 queue) {
  auto p = g_port.get(port);
  auto q = g_queue.get(queue);
  if (!p || !q) return CELL_ESRCH;
  std::lock_guard lk(p->m);
  if (p->queue) return CELL_EISCONN;
  p->queue = queue;
  return CELL_OK;
}
s32 sys_event_port_disconnect(u32 port) {
  auto p = g_port.get(port);
  if (!p) return CELL_ESRCH;
  std::lock_guard lk(p->m);
  if (!p->queue) return CELL_ENOTCONN;
  p->queue = 0;
  return CELL_OK;
}
s32 sys_event_port_send(u32 port, u64 d1, u64 d2, u64 d3) {
  auto p = g_port.get(port);
  if (!p) return CELL_ESRCH;
  u32 qid;
  u64 src;
  {
    std::lock_guard lk(p->m);
    qid = p->queue;
    src = p->name ? p->name : (u64(0x10000) << 32) | port;  // default source: port id
  }
  if (!qid) return CELL_ENOTCONN;
  return event_queue_send(qid, src, d1, d2, d3);
}

hle::SyscallTable s_sync({
    SYSCALL(100, sys_mutex_create),
    SYSCALL(101, sys_mutex_destroy),
    SYSCALL(102, sys_mutex_lock),
    SYSCALL(103, sys_mutex_trylock),
    SYSCALL(104, sys_mutex_unlock),
    SYSCALL(105, sys_cond_create),
    SYSCALL(106, sys_cond_destroy),
    SYSCALL(107, sys_cond_wait),
    SYSCALL(108, sys_cond_signal),
    SYSCALL(109, sys_cond_signal_all),
    SYSCALL(110, sys_cond_signal_to),
    SYSCALL(90, sys_semaphore_create),
    SYSCALL(91, sys_semaphore_destroy),
    SYSCALL(92, sys_semaphore_wait),
    SYSCALL(93, sys_semaphore_trywait),
    SYSCALL(94, sys_semaphore_post),
    SYSCALL(114, sys_semaphore_get_value),
    SYSCALL(120, sys_rwlock_create),
    SYSCALL(121, sys_rwlock_destroy),
    SYSCALL(122, sys_rwlock_rlock),
    SYSCALL(123, sys_rwlock_tryrlock),
    SYSCALL(124, sys_rwlock_runlock),
    SYSCALL(125, sys_rwlock_wlock),
    SYSCALL(126, sys_rwlock_trywlock),
    SYSCALL(127, sys_rwlock_wunlock),
    SYSCALL(128, sys_event_queue_create),
    SYSCALL(129, sys_event_queue_destroy),
    SYSCALL(130, sys_event_queue_receive),
    SYSCALL(131, sys_event_queue_tryreceive),
    SYSCALL(133, sys_event_queue_drain),
    SYSCALL(134, sys_event_port_create),
    SYSCALL(135, sys_event_port_destroy),
    SYSCALL(136, sys_event_port_connect_local),
    SYSCALL(137, sys_event_port_disconnect),
    SYSCALL(138, sys_event_port_send),
});

}  // namespace

s32 event_queue_send(u32 queue, u64 source, u64 d1, u64 d2, u64 d3) {
  auto q = g_queue.get(queue);
  if (!q) return CELL_ESRCH;
  {
    std::lock_guard lk(q->m);
    if (s32(q->events.size()) >= q->size) return CELL_EBUSY;
    q->events.push_back({source, d1, d2, d3});
  }
  q->cv.notify_one();
  return CELL_OK;
}

bool event_queue_exists(u32 queue) { return g_queue.get(queue) != nullptr; }
s32 event_queue_type(u32 queue) {
  auto q = g_queue.get(queue);
  return q ? q->type : 0;
}

s32 event_queue_receive_host(u32 queue, bool try_only, u64* src, u64* d1, u64* d2, u64* d3, std::atomic<bool>* cancel) {
  auto q = g_queue.get(queue);
  if (!q) return CELL_ESRCH;
  std::unique_lock lk(q->m);
  if (try_only && q->events.empty()) return CELL_EBUSY;
  ++q->waiters;
  while (q->events.empty() && !q->destroyed && !(cancel && cancel->load())) q->cv.wait_for(lk, std::chrono::milliseconds(5));
  --q->waiters;
  if (q->destroyed) return CELL_ECANCELED;
  if (q->events.empty()) return CELL_ECANCELED;
  Event e = q->events.front();
  q->events.pop_front();
  *src = e.source;
  *d1 = e.d1;
  *d2 = e.d2;
  *d3 = e.d3;
  return CELL_OK;
}

u32 event_queue_find_by_key(u64 key) {
  u32 found = 0;
  if (key) g_queue.for_each([&](u32 id, Queue& q) { if (q.key == key) found = id; });
  return found;
}

}  // namespace rt::lv2

namespace rt::lv2 {
// Logs held LV2 mutexes and the condition variables they belong to (diagnostics).
void sync_dump() {
  g_mutex.for_each([](u32 id, Mutex& mx) {
    std::lock_guard lk(mx.m);
    if (mx.o.owner)
      LOG_I("dump", "mutex 0x%x '%s' held by 0x%x (count %u, %zu waiting)", id, mx.name.c_str(), mx.o.owner, mx.o.count,
            mx.o.queue.size());
  });
  g_cond.for_each([](u32 id, Cond& c) {
    if (c.waiters())
      LOG_I("dump", "cond 0x%x '%s' (mutex '%s'): %u waiter(s)", id, c.name.c_str(), c.mx ? c.mx->name.c_str() : "?", c.waiters());
  });
  g_queue.for_each([](u32 id, Queue& q) {
    std::lock_guard lk(q.m);
    if (q.waiters || !q.events.empty())
      LOG_I("dump", "event queue 0x%x '%s' type %d: %zu event(s) pending (size %d), %u waiter(s)", id, q.name.c_str(), q.type,
            q.events.size(), q.size, q.waiters);
  });
}
}  // namespace rt::lv2
