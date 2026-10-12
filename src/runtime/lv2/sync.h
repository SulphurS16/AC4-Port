#pragma once
#include <atomic>

#include "common/bytes.h"

namespace rt::lv2 {

// Pushes an event into an LV2 event queue (used by event ports, SPU threads, audio, SPURS).
// Returns CELL_OK, CELL_ESRCH (no queue) or CELL_EBUSY (queue full).
s32 event_queue_send(u32 queue, u64 source, u64 d1, u64 d2, u64 d3);
bool event_queue_exists(u32 queue);
// Queue type (1 = PPU, 2 = SPU), or 0 if the queue does not exist.
s32 event_queue_type(u32 queue);
u32 event_queue_find_by_key(u64 ipc_key);  // 0 if none
// Host-side blocking receive (SPU receive_event syscall); cancel aborts the wait.
s32 event_queue_receive_host(u32 queue, bool try_only, u64* src, u64* d1, u64* d2, u64* d3, std::atomic<bool>* cancel);

}  // namespace rt::lv2
