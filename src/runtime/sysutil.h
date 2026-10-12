// State shared between cellSysutil and the presentation layer.
#pragma once
#include <functional>

#include "common/bytes.h"
#include "ppu/ppu_context.h"

namespace rt::sysutil {

struct VideoConfig {
  u8 resolution_id, format, aspect;
  u32 pitch;
};
void set_video_config(u8 res, u8 fmt, u8 aspect, u32 pitch);
VideoConfig video_config();

// Queues a system event (e.g. 0x0101 REQUEST_EXITGAME) for cellSysutilCheckCallback.
void post_event(u64 status, u64 param);
// Queues host work that must run on the game thread inside cellSysutilCheckCallback (e.g. an
// asynchronous dialog's or update check's guest callback).
void post_callback(std::function<void(ppu::Context&)> fn);

}  // namespace rt::sysutil
