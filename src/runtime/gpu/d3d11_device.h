// Direct3D 11 device, swap chain and presentation (owned by the RSX FIFO thread).
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>

#include <functional>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::gpu {

struct Device {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain1* swap = nullptr;
  ID3D11RenderTargetView* backbuffer_rtv = nullptr;
  u32 width = 1280, height = 720;
  bool no_overwrite_srv = false;  // D3D11.1: NO_OVERWRITE maps of dynamic buffers with shader resource views
};
Device& device();

bool device_init(void* hwnd);  // hwnd may be null (headless: no swap chain)
// Compiles HLSL with the OS d3dcompiler; returns nullptr and logs on failure.
// Shader compilation statistics of this run.
void shader_stats(u32* compiled, u32* cached, double* seconds);
ID3DBlob* compile_hlsl(const std::string& src, const char* entry, const char* target);

// Presents `srv` (any size) scaled to the window. `flip_y` for bottom-up sources.
void present_texture(ID3D11ShaderResourceView* srv, u32 src_w, u32 src_h);
// Copies the last presented frame to CPU memory (RGBA8) for captures (waits for the GPU).
bool read_back_last_frame(std::vector<u8>& rgba, u32& w, u32& h);
// Captures without stalling the frame: begin_capture copies the last presented frame into a
// staging texture; poll_captures (once per frame) reads back the copies the GPU has finished and
// passes their pixels to `done` (RGBA8, tightly packed).
void begin_capture(u32 index);
void poll_captures(const std::function<void(u32 index, std::vector<u8>&& rgba, u32 w, u32 h)>& done);
// Copies level 0 of a 4-byte-per-pixel texture to CPU memory (diagnostics).
bool read_back_texture(ID3D11Texture2D* tex, std::vector<u8>& rgba, u32& w, u32& h);

}  // namespace rt::gpu
