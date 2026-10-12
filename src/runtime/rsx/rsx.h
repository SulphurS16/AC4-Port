// RSX (GPU) front end: memory layout, IO mapping, FIFO processing, flips and vblank.
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt::rsx {

constexpr u32 kLocalBase = 0xC0000000;
// RSX device area (one 256 MiB address slot reserved at init), LV2 layout:
constexpr u32 kDeviceBase = 0x40000000;
constexpr u32 kContextAddr = kDeviceBase;               // CellGcmContextData (default context)
constexpr u32 kDmaControl = kDeviceBase + 0x100000;     // RsxDmaControl; CellGcmControl at +0x40
constexpr u32 kControlAddr = kDmaControl + 0x40;        // put/get/ref
constexpr u32 kDriverInfo = kDeviceBase + 0x200000;
constexpr u32 kReportsBase = kDeviceBase + 0x300000;    // RsxReports: semaphores(labels), notify, report
constexpr u32 kLabelAddr = kReportsBase;                // 256 labels, 16-byte stride
constexpr u32 kNotifyAddr = kReportsBase + 0x1000;
constexpr u32 kReportAddr = kReportsBase + 0x1400;      // 2048 reports, 16 bytes each
constexpr u32 kHleData = kDeviceBase + 0x400000;        // runtime-owned gcm structures
constexpr u32 kDeviceSize = 0x500000;

void init(u32 io_ea, u32 io_size, u32 local_size);
bool initialized();
u32 local_size();

// IO mapping (main memory visible to RSX), 1 MiB granularity.
bool map_io(u32 ea, u32 io, u32 size);
bool unmap_io(u32 io, u32 size);
bool io_to_ea(u32 io, u32* ea);
bool ea_to_io(u32 ea, u32* io);
// RSX offset + location (0 = local, 1 = main/IO) -> guest effective address. Returns 0 if invalid.
u32 offset_to_ea(u32 offset, u32 location);

struct DisplayBuffer {
  u32 offset = 0, pitch = 0, width = 0, height = 0;
};
void set_display_buffer(u32 id, const DisplayBuffer& b);

// Flip / vblank state (cellGcm*Flip*).
u32 flip_status();
void reset_flip_status();
u64 last_flip_time();
void set_flip_mode(u32 mode);
void set_flip_handler(u32 opd);
void set_vblank_handler(u32 opd);
u64 vblank_count();

// Starts the RSX command processor and vblank threads (called once by cellGcmInit).
void start();

// Debug statistics.
std::string stats();
u64 flips();  // flips executed so far
// Renderer switches for A/B measurements ("gpu NAME VALUE" control command): "submit on|off"
// (D3D11 calls on their own thread or on the FIFO thread). False for an unknown name or value.
bool gpu_option(const std::string& name, const std::string& value);
void dump();  // diagnostics: FIFO pointers, pending semaphore acquire, last methods

}  // namespace rt::rsx
