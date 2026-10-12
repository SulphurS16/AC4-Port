// D3D11 immediate-context work of the renderer, recorded on the RSX FIFO thread and executed in
// order on a submission thread ("rsx-submit"). The driver spends about half of the FIFO thread's
// time inside context calls (state validation and command building per draw); with the calls on
// their own thread that cost overlaps the FIFO thread's work (method execution, vertex
// conversion, texture decoding), which stays where the guest's memory is read, in RSX order.
//
// The recorder mirrors the subset of ID3D11DeviceContext the renderer uses (same names and
// signatures), so call sites read like immediate-context code. Data passed by pointer is copied
// when recorded. Objects are created with the device on the recording thread (thread-safe); an
// object referenced by recorded calls is released through release(), after those calls ran.
// Synchronous mode executes every call at once on the calling thread (the previous behaviour;
// also the fallback). "gpu submit on|off" switches at a frame boundary (A/B measurements).
#pragma once
#include <d3d11.h>

#include <functional>
#include <string>

#include "common/bytes.h"

namespace rt::gpu {

class Recorder {
 public:
  void OMSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
  void OMSetBlendState(ID3D11BlendState* s, const FLOAT factor[4], UINT mask);
  void OMSetDepthStencilState(ID3D11DepthStencilState* s, UINT ref);
  void RSSetState(ID3D11RasterizerState* s);
  void RSSetViewports(UINT n, const D3D11_VIEWPORT* vps);
  void RSSetScissorRects(UINT n, const D3D11_RECT* rects);
  void IASetInputLayout(ID3D11InputLayout* layout);
  void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topo);
  void IASetIndexBuffer(ID3D11Buffer* b, DXGI_FORMAT format, UINT offset);
  void VSSetShader(ID3D11VertexShader* s, ID3D11ClassInstance* const* ci, UINT nci);
  void PSSetShader(ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, UINT nci);
  void VSSetConstantBuffers(UINT slot, UINT n, ID3D11Buffer* const* bufs);
  void PSSetConstantBuffers(UINT slot, UINT n, ID3D11Buffer* const* bufs);
  void VSSetShaderResources(UINT slot, UINT n, ID3D11ShaderResourceView* const* views);
  void PSSetShaderResources(UINT slot, UINT n, ID3D11ShaderResourceView* const* views);
  void PSSetSamplers(UINT slot, UINT n, ID3D11SamplerState* const* samplers);
  void Draw(UINT count, UINT start);
  void DrawIndexed(UINT count, UINT start, INT base_vertex);
  void ClearRenderTargetView(ID3D11RenderTargetView* v, const FLOAT color[4]);
  void ClearDepthStencilView(ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil);
  void CopyResource(ID3D11Resource* dst, ID3D11Resource* src);
  // `bytes` of `data` are copied (the whole update: row pitch x rows x depth for textures).
  void UpdateSubresource(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch,
                         size_t bytes);
  // Map (WRITE_DISCARD or WRITE_NO_OVERWRITE), copy `bytes` to `offset`, Unmap (dynamic buffers).
  void write_buffer(ID3D11Buffer* b, D3D11_MAP mode, u32 offset, const void* data, u32 bytes);
  // Room for `bytes` that write_buffer would copy, filled by the caller before the next call
  // (avoids a copy: e.g. vertices converted straight into the recorded data).
  void* write_buffer_space(ID3D11Buffer* b, D3D11_MAP mode, u32 offset, u32 bytes);
  // Releases `o` once the calls recorded before have run (null is ignored).
  void release(IUnknown* o);
  // Runs `fn` with the immediate context, in order (presentation, captures, multi-step passes).
  void call(std::function<void(ID3D11DeviceContext*)> fn);
  // Runs `fn` in order and waits for it (diagnostics that read results back).
  void call_sync(const std::function<void(ID3D11DeviceContext*)>& fn);

  // Hands the recorded calls to the submission thread (cheap when nothing is pending).
  void flush();
  // End of a frame: flushes, then waits while more than `max_frames` frames are queued.
  void frame_end(u32 max_frames = 1);
  // Waits until everything recorded has run.
  void finish();
  // After a draw: hands the recorded calls over when the submission thread is idle or the batch is
  // large, so it starts early without a handover per call.
  void kick();
  bool async() const;
};

Recorder& rec();
// Starts the submission thread (after the device exists). Synchronous mode if `async` is false.
void submit_init(bool async);
// "gpu submit on|off" (applied at the next frame boundary by apply_submit_mode).
void request_submit_mode(bool async);
void apply_submit_mode();
// Submission thread statistics for the perf lines: milliseconds spent executing calls and CPU
// cycles used (OS thread cycle count), both totals since start.
void submit_stats(double* busy_ms, u64* cpu_cycles);
// Recorded-call memory (chunks allocated, their capacity, chunks waiting) for the perf lines.
std::string submit_memory();

}  // namespace rt::gpu
