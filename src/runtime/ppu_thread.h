// Guest PPU threads: each runs recompiled code on its own host thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "ppu/ppu_context.h"

namespace rt {

struct PpuThread {
  u32 id = 0;
  std::string name;
  alignas(64) ppu::Context ctx{};
  u32 entry_opd = 0;
  u64 arg = 0;
  s32 prio = 0;
  u32 stack_addr = 0, stack_size = 0;
  u32 tls_block = 0;
  bool detached = false;
  std::atomic<int> state{0};  // 0 created, 1 running, 2 exited
  u64 exit_status = 0;
  void* host = nullptr;       // HANDLE
  u32 host_tid = 0;
  std::mutex mu;
  std::condition_variable cv;
  bool join_claimed = false;
  std::function<void(ppu::Context&)> host_main;  // runtime-owned threads (callback dispatchers)
  // Set while the creating thread waits for this (higher-priority) thread to block or exit.
  std::atomic<bool> handoff{false};
  // Diagnostics: the HLE function or syscall this thread is currently inside (nullptr = guest code).
  std::atomic<const char*> in_call{nullptr};
  u64 call_args[4] = {};
  u32 call_lr = 0;
};

// Records/clears the call a thread is inside (for thread_dump).
struct CallScope {
  PpuThread* t;
  CallScope(const char* name, const ppu::Context& ctx);
  ~CallScope();
};
// Logs every thread's state (diagnostics at shutdown or on a stall).
void thread_dump();
// Called when the current PPU thread is about to block (wait or sleep). LV2 runs a newly started
// thread of higher priority than its creator until it blocks; thread_start models that by
// letting the creator wait for this notification.
void note_block();

// Thrown to unwind a guest thread's host stack on sys_ppu_thread_exit.
struct ThreadExit {
  u64 status;
};

PpuThread* cur_thread();
PpuThread* thread_get(u32 id);

// Creates a thread (not started). Returns its id or 0 with *err set.
u32 thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stack_size, bool joinable, const std::string& name, s32* err);
s32 thread_start(u32 id);
[[noreturn]] void thread_exit(u64 status);
s32 thread_join(u32 id, u64* status);
s32 thread_detach(u32 id);
void thread_for_each(void (*fn)(PpuThread&, void*), void* user);

// Creates and starts a runtime-owned guest thread whose body is host code (it may call guest
// functions through hle::call_guest with its own context and guest stack).
u32 thread_create_host(const std::string& name, s32 prio, u32 stack_size, std::function<void(ppu::Context&)> body);

// Starts the primary thread at the ELF entry point (sys_initialize_tls first, then _start).
void start_main_thread(u32 entry_opd, u32 stack_size, s32 prio, u32 tls_vaddr, u32 tls_filesz, u32 tls_memsz,
                       u32 malloc_pagesize, const std::string& argv0);

// TLS (sys_initialize_tls / per-thread blocks).
void tls_initialize(ppu::Context& ctx, u32 image, u32 filesz, u32 memsz);
u32 tls_alloc();
void tls_free(u32 block);

// Process termination (sys_process_exit and friends).
[[noreturn]] void process_exit(s32 status);
bool process_exiting();

}  // namespace rt
