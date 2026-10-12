#pragma once
#include <vector>

#include "runtime/spu/spu.h"

namespace rt::spu {

// sys_spu_image_import / sys_spu_image_close (liblv2 behaviour, §9.2-§9.3).
s32 image_import(u32 img, u32 src, u32 type);
s32 image_close(u32 img);
// Entry point and loadable segments of a sys_spu_image (validated as sys_spu_thread_initialize
// does, §9.4). INFO segments are dropped.
s32 image_segments(u32 img, u32* entry, std::vector<Segment>* out);

}  // namespace rt::spu
