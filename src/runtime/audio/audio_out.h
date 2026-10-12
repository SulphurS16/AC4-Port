// Host audio output (WASAPI shared mode, 48 kHz float stereo with OS-side conversion).
#pragma once
#include "common/bytes.h"

namespace rt::audio {

bool out_init();
bool out_available();
// Frames currently queued in the device buffer (0 if unavailable).
u32 out_queued_frames();
// Writes interleaved stereo float frames (non-blocking; drops if the device buffer is full).
void out_write(const float* stereo, u32 frames);

}  // namespace rt::audio
