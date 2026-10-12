#include "runtime/ppu_thread.h"
#include "runtime/profiler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <deque>
#include <map>
#include <vector>

#include "ppu/ppu_recomp.h"
#include "runtime/hle.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"

namespace rt {
namespace {

thread_local PpuThread* t_cur = nullptr;
std::mutex g_mu;
std::map<u32, std::unique_ptr<PpuThread>> g_threads;
u32 g_next_id = 0x01000000;
std::atomic<bool> g_exiting{false};

// TLS layout (LV2 / liblv2): per-thread block = 0x30-byte system area + TLS image; r13 points
// 0x7000 past the image start.
std::mutex g_tls_mu;
u32 g_tls_image = 0, g_tls_file = 0, g_tls_zero = 0, g_tls_size = 0;
bool g_tls_init = false;

constexpr u32 kStackStartOffset = 0x70;

DWORD WINAPI thread_main(LPVOID p) {
  PpuThread* t = static_cast<PpuThread*>(p);
  t_cur = t;
  t->state = 1;
  u64 status = 0;
  try {
    if (t->host_main) {
      t->host_main(t->ctx);
      throw ThreadExit{0};
    }
    u32 code = ppu::r32(t->entry_opd), toc = ppu::r32(t->entry_opd + 4);
    t->ctx.r[2] = toc;
    t->ctx.lr = 0;
    ppu::icall(t->ctx, code);
    status = t->ctx.r[3];
    LOG_D("thread", "thread 0x%x '%s' returned from entry (r3=0x%llx)", t->id, t->name.c_str(), (unsigned long long)status);
  } catch (const ThreadExit& e) {
    status = e.status;
  }
  {
    std::lock_guard lk(t->mu);
    t->exit_status = status;
    t->state = 2;
    t->handoff = false;
  }
  t->cv.notify_all();
  LOG_I("thread", "thread 0x%x '%s' exited (status 0x%llx)", t->id, t->name.c_str(), (unsigned long long)status);
  if (t->tls_block) tls_free(t->tls_block);
  return 0;
}

}  // namespace

PpuThread* cur_thread() { return t_cur; }

PpuThread* thread_get(u32 id) {
  std::lock_guard lk(g_mu);
  auto it = g_threads.find(id);
  return it == g_threads.end() ? nullptr : it->second.get();
}

CallScope::CallScope(const char* name, const ppu::Context& ctx) : t(t_cur) {
  if (!t) return;
  for (int i = 0; i < 4; ++i) t->call_args[i] = ctx.r[3 + i];
  t->call_lr = u32(ctx.lr);
  t->in_call.store(name, std::memory_order_release);
}
CallScope::~CallScope() {
  if (t) t->in_call.store(nullptr, std::memory_order_release);
}

void thread_dump() {
  std::lock_guard lk(g_mu);
  for (auto& [id, t] : g_threads) {
    const char* call = t->in_call.load(std::memory_order_acquire);
    const char* st = t->state == 0 ? "created" : t->state == 1 ? "running" : "exited";
    if (call)
      LOG_I("dump", "PPU 0x%x '%s' %s: in %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) lr 0x%08x", id, t->name.c_str(), st, call,
            (unsigned long long)t->call_args[0], (unsigned long long)t->call_args[1], (unsigned long long)t->call_args[2],
            (unsigned long long)t->call_args[3], t->call_lr);
    else
      LOG_I("dump", "PPU 0x%x '%s' %s: in guest code", id, t->name.c_str(), st);
  }
}

void thread_for_each(void (*fn)(PpuThread&, void*), void* user) {
  std::lock_guard lk(g_mu);
  for (auto& [id, t] : g_threads) fn(*t, user);
}

void tls_initialize(ppu::Context& ctx, u32 image, u32 filesz, u32 memsz) {
  {
    std::lock_guard lk(g_tls_mu);
    if (!g_tls_init) {
      g_tls_image = image;
      g_tls_file = filesz;
      g_tls_zero = memsz - filesz;
      g_tls_size = (memsz + 0x30 + 0x7f) & ~0x7fu;
      g_tls_init = true;
      LOG_I("tls", "TLS initialized: image 0x%x file 0x%x mem 0x%x (block 0x%x)", image, filesz, memsz, g_tls_size);
    }
  }
  if (ctx.r[13] == 0) {
    u32 b = tls_alloc();
    if (PpuThread* t = cur_thread()) t->tls_block = b;
    ctx.r[13] = b + 0x30 + 0x7000;
  }
}

// TLS blocks stay mapped once allocated: an exited thread's block is queued and handed, re-
// initialized, to a later thread (oldest freed first), as LV2's fixed TLS slots behave, so a
// late read of an exited thread's TLS sees its old contents instead of faulting. (Tried as the
// cause of the mission-start crash in the sound lookup; that crash still occurs, see STATUS.)
std::deque<u32> g_tls_free;

u32 tls_alloc() {
  std::lock_guard lk(g_tls_mu);
  if (!g_tls_init) return 0;
  u32 b;
  if (!g_tls_free.empty()) {
    b = g_tls_free.front();
    g_tls_free.pop_front();
  } else {
    b = vm::alloc(vm::Area::Main, g_tls_size, 0x80);
    if (!b) fatal("TLS block allocation failed");
  }
  std::memset(ppu::mem() + b, 0, 0x30);
  std::memcpy(ppu::mem() + b + 0x30, ppu::mem() + g_tls_image, g_tls_file);
  std::memset(ppu::mem() + b + 0x30 + g_tls_file, 0, g_tls_zero);
  return b;
}

void tls_free(u32 block) {
  std::lock_guard lk(g_tls_mu);
  g_tls_free.push_back(block);
}

u32 thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stack_size, bool joinable, const std::string& name, s32* err) {
  auto t = std::make_unique<PpuThread>();
  if (stack_size < 0x4000) stack_size = 0x4000;
  stack_size = (stack_size + 0xfff) & ~0xfffu;
  t->stack_size = stack_size;
  t->stack_addr = vm::alloc(vm::Area::Stack, stack_size, 0x1000);
  if (!t->stack_addr) {
    if (err) *err = CELL_ENOMEM;
    return 0;
  }
  t->entry_opd = entry_opd;
  t->arg = arg;
  t->prio = prio;
  t->name = name;
  t->detached = !joinable;
  t->ctx.r[1] = t->stack_addr + stack_size - kStackStartOffset;
  t->ctx.r[3] = arg;
  t->ctx.vscr = 0x10000;  // NJ
  if (g_tls_init) {
    t->tls_block = tls_alloc();
    t->ctx.r[13] = t->tls_block + 0x30 + 0x7000;
  }
  std::lock_guard lk(g_mu);
  t->id = g_next_id++;
  t->ctx.thread = t.get();
  u32 id = t->id;
  g_threads[id] = std::move(t);
  return id;
}

void note_block() {
  PpuThread* t = t_cur;
  if (!t || !t->handoff.load(std::memory_order_acquire)) return;
  {
    std::lock_guard lk(t->mu);
    t->handoff = false;
  }
  t->cv.notify_all();
}

s32 thread_start(u32 id) {
  PpuThread* t = thread_get(id);
  if (!t) return CELL_ESRCH;
  PpuThread* creator = t_cur;
  // LV2 schedules by priority (lower value = higher priority): a new thread that outranks its
  // creator runs first, until it blocks. Games rely on this ordering during initialization.
  bool handoff = creator && !creator->host_main && !t->host_main && t->prio < creator->prio;
  t->handoff = handoff;
  DWORD tid = 0;
  HANDLE h = CreateThread(nullptr, 64u << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
  if (!h) {
    t->handoff = false;
    return CELL_EAGAIN;
  }
  t->host = h;
  t->host_tid = tid;
  std::wstring wname(t->name.begin(), t->name.end());
  SetThreadDescription(h, (L"ppu:" + wname).c_str());
  prof::register_thread("ppu:" + t->name, h);
  LOG_I("thread", "thread 0x%x '%s' started (entry opd 0x%x, prio %d, stack 0x%x+0x%x, tls 0x%x)", t->id, t->name.c_str(),
        t->entry_opd, t->prio, t->stack_addr, t->stack_size, t->tls_block);
  input_thread_started(t->name);  // anchors for scripted input
  if (handoff) {
    std::unique_lock lk(t->mu);
    if (!t->cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return !t->handoff.load(); })) {
      t->handoff = false;
      LOG_D("thread", "thread 0x%x '%s' ran 20 ms without blocking; creator continues", t->id, t->name.c_str());
    }
  }
  return CELL_OK;
}

void thread_exit(u64 status) { throw ThreadExit{status}; }

u32 thread_create_host(const std::string& name, s32 prio, u32 stack_size, std::function<void(ppu::Context&)> body) {
  s32 err = 0;
  u32 id = thread_create(0, 0, prio, stack_size, false, name, &err);
  if (!id) fatal("cannot create runtime thread " + name);
  thread_get(id)->host_main = std::move(body);
  thread_start(id);
  return id;
}

s32 thread_join(u32 id, u64* status) {
  PpuThread* t = thread_get(id);
  if (!t) return CELL_ESRCH;
  if (t == cur_thread()) return CELL_EDEADLK;
  std::unique_lock lk(t->mu);
  if (t->detached || t->join_claimed) return CELL_EINVAL;
  t->join_claimed = true;
  if (t->state != 2) note_block();
  t->cv.wait(lk, [&] { return t->state == 2; });
  if (status) *status = t->exit_status;
  return CELL_OK;
}

s32 thread_detach(u32 id) {
  PpuThread* t = thread_get(id);
  if (!t) return CELL_ESRCH;
  std::lock_guard lk(t->mu);
  if (t->detached) return CELL_EINVAL;
  if (t->join_claimed) return CELL_EBUSY;
  t->detached = true;
  return CELL_OK;
}

void start_main_thread(u32 entry_opd, u32 stack_size, s32 prio, u32 tls_vaddr, u32 tls_filesz, u32 tls_memsz,
                       u32 malloc_pagesize, const std::string& argv0) {
  s32 err = 0;
  u32 id = thread_create(entry_opd, 0, prio, stack_size, true, "main_thread", &err);
  if (!id) fatal("cannot create main thread");
  PpuThread* t = thread_get(id);
  // argv/envp on the primary stack (pointer array of u64, then strings, 16-byte aligned).
  u32 str_size = (u32(argv0.size()) + 1 + 15) & ~15u;
  u32 ptrs = 8 * 4;  // argv[0], NULL, envp NULL, pad
  u32 alloc = ptrs + str_size;
  u32 args = t->stack_addr + t->stack_size - alloc;
  u32 argv = args, envp = args + 16, s = args + ptrs;
  std::memcpy(ppu::mem() + s, argv0.c_str(), argv0.size() + 1);
  ppu::w64(argv, s);
  ppu::w64(argv + 8, 0);
  ppu::w64(envp, 0);
  t->ctx.r[1] = args - kStackStartOffset;
  t->ctx.r[13] = 0;
  // sys_initialize_tls as liblv2 would, then the entry with LV2's process arguments.
  tls_initialize(t->ctx, tls_vaddr, tls_filesz, tls_memsz);
  t->tls_block = t->ctx.r[13] - 0x7030;
  t->ctx.r[3] = 1;
  t->ctx.r[4] = argv;
  t->ctx.r[5] = envp;
  t->ctx.r[6] = 0;
  t->ctx.r[7] = id;
  t->ctx.r[8] = tls_vaddr;
  t->ctx.r[9] = tls_filesz;
  t->ctx.r[10] = tls_memsz;
  t->ctx.r[11] = entry_opd;
  t->ctx.r[12] = malloc_pagesize;
  if (thread_start(id) != CELL_OK) fatal("cannot start main thread");
}

void process_exit(s32 status) {
  g_exiting = true;
  LOG_I("process", "guest process exit (status %d)", status);
  log_flush();
  ExitProcess(UINT(status));
}

bool process_exiting() { return g_exiting; }

}  // namespace rt
