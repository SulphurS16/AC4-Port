// Presentation timing shared by the RSX flip path (rsx_core.cpp do_flip), the presenter
// (d3d11_device.cpp) and the window (window.cpp). No D3D headers.
#pragma once
#include "common/bytes.h"

namespace rt::gpu {

// Refresh rate of the display the window is on, in Hz (0 when unknown or headless). Updated when
// the window moves to another display or the display mode changes.
double display_refresh_hz();
// Sync interval of the following presents: k >= 1 shows each frame for at least k refreshes of the
// display (vsync); 0 presents at once (tearing allowed where the system supports it).
void set_present_interval(u32 k);
// Waits until the swap chain takes another frame without queueing it behind one that is not yet
// on screen (maximum frame latency 1): with a sync interval, this returns as the previous frame
// reaches the display. False after `timeout_ms` or without a swap chain.
bool wait_present_ready(u32 timeout_ms);
// Diagnostics: writes the next presented image as the window shows it (scaled, with its borders)
// to the run directory (output_NNNN.png).
void request_output_capture();

}  // namespace rt::gpu
