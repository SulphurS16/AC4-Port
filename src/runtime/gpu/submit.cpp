#include "runtime/gpu/submit.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/gpu/d3d11_device.h"
#include "runtime/log.h"
#include "runtime/profiler.h"

namespace rt::gpu {
namespace {

enum class Op : u32 {
  OMSetRenderTargets,
  OMSetBlendState,
  OMSetDepthStencilState,
  RSSetState,
  RSSetViewports,
  RSSetScissorRects,
  IASetInputLayout,
  IASetPrimitiveTopology,
  IASetIndexBuffer,
  VSSetShader,
  PSSetShader,
  VSSetConstantBuffers,
  PSSetConstantBuffers,
  VSSetShaderResources,
  PSSetShaderResources,
  PSSetSamplers,
  Draw,
  DrawIndexed,
  ClearRenderTargetView,
  ClearDepthStencilView,
  CopyResource,
  UpdateSubresource,
  WriteBuffer,
  Release,
  Call,
};

struct Hdr {
  Op op;
  u32 size;  // the whole command, header included (multiple of 8)
};

// Recorded calls: commands packed back to back (no per-call allocation), plus the closures of
// Call commands.
struct Chunk {
  u8* data = nullptr;
  size_t size = 0, cap = 0;
  std::vector<std::function<void(ID3D11DeviceContext*)>> fns;
  u32 frames = 0;  // frame ends recorded in this chunk
  ~Chunk() { std::free(data); }
  void reserve(size_t n) {
    if (n <= cap) return;
    size_t c = std::max<size_t>(n, cap ? cap * 2 : size_t(256) << 10);
    data = static_cast<u8*>(std::realloc(data, c));
    if (!data) std::abort();
    cap = c;
  }
  void clear() {
    size = 0;
    fns.clear();
    frames = 0;
  }
};

std::mutex g_mu;
std::condition_variable g_cv_work, g_cv_done;
std::deque<std::unique_ptr<Chunk>> g_queue;
std::vector<std::unique_ptr<Chunk>> g_free;
std::unique_ptr<Chunk> g_cur;  // recording (FIFO thread)
bool g_async = false;          // mode in effect (FIFO thread)
bool g_started = false;
std::atomic<int> g_want{-1};   // requested mode (-1: none)
u64 g_chunks_sent = 0;         // FIFO thread
u64 g_frames_sent = 0;         // FIFO thread
std::atomic<u64> g_chunks_done{0}, g_frames_done{0};
std::atomic<bool> g_idle{true};
std::atomic<u64> g_busy_ticks{0};
size_t g_queued_bytes = 0;  // recorded bytes waiting for the submission thread (under g_mu)
// Backpressure: the recording thread waits while more than this is queued (between frame ends it
// could otherwise run ahead without bound, e.g. through loading sequences), and spare chunks
// beyond a few are freed instead of kept.
constexpr size_t kMaxQueuedBytes = size_t(32) << 20;
constexpr size_t kMaxQueuedChunks = 64;
constexpr size_t kMaxFreeChunks = 64;
constexpr size_t kMaxKeptCapacity = size_t(1) << 20;
std::atomic<HANDLE> g_thread{nullptr};  // the submission thread (cycle counts)
ID3D11Buffer* g_sync_unmap = nullptr;  // synchronous write_buffer_space: unmapped at the next call

ID3D11DeviceContext* ctx() { return device().ctx; }

void sync_pending() {
  if (g_sync_unmap) {
    ctx()->Unmap(g_sync_unmap, 0);
    g_sync_unmap = nullptr;
  }
}

template <class T>
T* emit(Op op, size_t extra = 0) {
  size_t size = (sizeof(Hdr) + sizeof(T) + extra + 7) & ~size_t(7);
  Chunk& c = *g_cur;
  c.reserve(c.size + size);
  auto* h = reinterpret_cast<Hdr*>(c.data + c.size);
  h->op = op;
  h->size = u32(size);
  c.size += size;
  return reinterpret_cast<T*>(h + 1);
}
template <class T>
u8* payload(T* args) {
  return reinterpret_cast<u8*>(args + 1);
}

struct ViewsArgs {
  UINT slot, n;
};  // followed by n pointers
struct RtArgs {
  UINT n;
  ID3D11DepthStencilView* dsv;
};  // followed by n RTV pointers
struct BlendArgs {
  ID3D11BlendState* s;
  FLOAT factor[4];
  bool has_factor;
  UINT mask;
};
struct DepthArgs {
  ID3D11DepthStencilState* s;
  UINT ref;
};
struct PtrArgs {
  void* p;
};
struct CountArgs {
  UINT n;
};  // followed by n items
struct TopoArgs {
  D3D11_PRIMITIVE_TOPOLOGY t;
};
struct IbArgs {
  ID3D11Buffer* b;
  DXGI_FORMAT f;
  UINT off;
};
struct DrawArgs {
  UINT count, start;
  INT base;
};
struct ClearRtArgs {
  ID3D11RenderTargetView* v;
  FLOAT c[4];
};
struct ClearDsArgs {
  ID3D11DepthStencilView* v;
  UINT flags;
  FLOAT d;
  UINT8 s;
};
struct CopyArgs {
  ID3D11Resource* dst;
  ID3D11Resource* src;
};
struct UpdateArgs {
  ID3D11Resource* r;
  UINT sub;
  bool has_box;
  D3D11_BOX box;
  UINT row, depth;
  size_t bytes;
};  // followed by the data
struct WriteArgs {
  ID3D11Buffer* b;
  D3D11_MAP mode;
  u32 offset, bytes;
};  // followed by the data
struct CallArgs {
  u32 index;
};

void execute(Chunk& c, ID3D11DeviceContext* x) {
  for (size_t pos = 0; pos < c.size;) {
    auto* h = reinterpret_cast<Hdr*>(c.data + pos);
    void* a = h + 1;
    switch (h->op) {
      case Op::OMSetRenderTargets: {
        auto* r = static_cast<RtArgs*>(a);
        x->OMSetRenderTargets(r->n, reinterpret_cast<ID3D11RenderTargetView* const*>(payload(r)), r->dsv);
        break;
      }
      case Op::OMSetBlendState: {
        auto* r = static_cast<BlendArgs*>(a);
        x->OMSetBlendState(r->s, r->has_factor ? r->factor : nullptr, r->mask);
        break;
      }
      case Op::OMSetDepthStencilState: {
        auto* r = static_cast<DepthArgs*>(a);
        x->OMSetDepthStencilState(r->s, r->ref);
        break;
      }
      case Op::RSSetState: x->RSSetState(static_cast<ID3D11RasterizerState*>(static_cast<PtrArgs*>(a)->p)); break;
      case Op::RSSetViewports: {
        auto* r = static_cast<CountArgs*>(a);
        x->RSSetViewports(r->n, reinterpret_cast<const D3D11_VIEWPORT*>(payload(r)));
        break;
      }
      case Op::RSSetScissorRects: {
        auto* r = static_cast<CountArgs*>(a);
        x->RSSetScissorRects(r->n, reinterpret_cast<const D3D11_RECT*>(payload(r)));
        break;
      }
      case Op::IASetInputLayout: x->IASetInputLayout(static_cast<ID3D11InputLayout*>(static_cast<PtrArgs*>(a)->p)); break;
      case Op::IASetPrimitiveTopology: x->IASetPrimitiveTopology(static_cast<TopoArgs*>(a)->t); break;
      case Op::IASetIndexBuffer: {
        auto* r = static_cast<IbArgs*>(a);
        x->IASetIndexBuffer(r->b, r->f, r->off);
        break;
      }
      case Op::VSSetShader: x->VSSetShader(static_cast<ID3D11VertexShader*>(static_cast<PtrArgs*>(a)->p), nullptr, 0); break;
      case Op::PSSetShader: x->PSSetShader(static_cast<ID3D11PixelShader*>(static_cast<PtrArgs*>(a)->p), nullptr, 0); break;
      case Op::VSSetConstantBuffers: {
        auto* r = static_cast<ViewsArgs*>(a);
        x->VSSetConstantBuffers(r->slot, r->n, reinterpret_cast<ID3D11Buffer* const*>(payload(r)));
        break;
      }
      case Op::PSSetConstantBuffers: {
        auto* r = static_cast<ViewsArgs*>(a);
        x->PSSetConstantBuffers(r->slot, r->n, reinterpret_cast<ID3D11Buffer* const*>(payload(r)));
        break;
      }
      case Op::VSSetShaderResources: {
        auto* r = static_cast<ViewsArgs*>(a);
        x->VSSetShaderResources(r->slot, r->n, reinterpret_cast<ID3D11ShaderResourceView* const*>(payload(r)));
        break;
      }
      case Op::PSSetShaderResources: {
        auto* r = static_cast<ViewsArgs*>(a);
        x->PSSetShaderResources(r->slot, r->n, reinterpret_cast<ID3D11ShaderResourceView* const*>(payload(r)));
        break;
      }
      case Op::PSSetSamplers: {
        auto* r = static_cast<ViewsArgs*>(a);
        x->PSSetSamplers(r->slot, r->n, reinterpret_cast<ID3D11SamplerState* const*>(payload(r)));
        break;
      }
      case Op::Draw: {
        auto* r = static_cast<DrawArgs*>(a);
        x->Draw(r->count, r->start);
        break;
      }
      case Op::DrawIndexed: {
        auto* r = static_cast<DrawArgs*>(a);
        x->DrawIndexed(r->count, r->start, r->base);
        break;
      }
      case Op::ClearRenderTargetView: {
        auto* r = static_cast<ClearRtArgs*>(a);
        x->ClearRenderTargetView(r->v, r->c);
        break;
      }
      case Op::ClearDepthStencilView: {
        auto* r = static_cast<ClearDsArgs*>(a);
        x->ClearDepthStencilView(r->v, r->flags, r->d, r->s);
        break;
      }
      case Op::CopyResource: {
        auto* r = static_cast<CopyArgs*>(a);
        x->CopyResource(r->dst, r->src);
        break;
      }
      case Op::UpdateSubresource: {
        auto* r = static_cast<UpdateArgs*>(a);
        x->UpdateSubresource(r->r, r->sub, r->has_box ? &r->box : nullptr, payload(r), r->row, r->depth);
        break;
      }
      case Op::WriteBuffer: {
        auto* r = static_cast<WriteArgs*>(a);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (HRESULT hr = x->Map(r->b, 0, r->mode, 0, &ms); SUCCEEDED(hr)) {
          std::memcpy(static_cast<u8*>(ms.pData) + r->offset, payload(r), r->bytes);
          x->Unmap(r->b, 0);
        } else {
          static std::atomic<int> warned{0};
          if (warned++ < 4) LOG_W("gpu", "buffer map failed (0x%08lx, mode %d): %u bytes not written", hr, int(r->mode), r->bytes);
        }
        break;
      }
      case Op::Release: static_cast<IUnknown*>(static_cast<PtrArgs*>(a)->p)->Release(); break;
      case Op::Call: c.fns[static_cast<CallArgs*>(a)->index](x); break;
    }
    pos += h->size;
  }
}

void submit_thread() {
  SetThreadDescription(GetCurrentThread(), L"rsx-submit");
  g_thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
  prof::register_thread("rsx-submit", OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                                   GetCurrentThreadId()));
  for (;;) {
    std::unique_ptr<Chunk> c;
    {
      std::unique_lock lk(g_mu);
      g_idle = true;
      g_cv_work.wait(lk, [] { return !g_queue.empty(); });
      c = std::move(g_queue.front());
      g_queue.pop_front();
      g_queued_bytes -= c->size;
      g_idle = false;
    }
    g_cv_done.notify_all();  // room in the queue
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    execute(*c, ctx());
    QueryPerformanceCounter(&t1);
    g_busy_ticks.fetch_add(u64(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    u32 frames = c->frames;
    c->clear();
    {
      std::lock_guard lk(g_mu);
      if (g_free.size() < kMaxFreeChunks && c->cap <= kMaxKeptCapacity) g_free.push_back(std::move(c));
      g_frames_done.fetch_add(frames, std::memory_order_relaxed);
      g_chunks_done.fetch_add(1, std::memory_order_release);
    }
    g_cv_done.notify_all();
  }
}

std::atomic<u32> g_chunks_made{0};

std::unique_ptr<Chunk> take_chunk() {
  std::lock_guard lk(g_mu);
  if (g_free.empty()) {
    ++g_chunks_made;
    return std::make_unique<Chunk>();
  }
  auto c = std::move(g_free.back());
  g_free.pop_back();
  return c;
}

}  // namespace

Recorder& rec() {
  static Recorder r;
  return r;
}

bool Recorder::async() const { return g_async; }

#define SYNC_CALL(expr)   \
  if (!g_async) {         \
    sync_pending();       \
    ctx()->expr;          \
    return;               \
  }

void Recorder::OMSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
  SYNC_CALL(OMSetRenderTargets(n, rtvs, dsv));
  auto* r = emit<RtArgs>(Op::OMSetRenderTargets, n * sizeof(void*));
  r->n = n;
  r->dsv = dsv;
  if (n) std::memcpy(payload(r), rtvs, n * sizeof(void*));
}
void Recorder::OMSetBlendState(ID3D11BlendState* s, const FLOAT factor[4], UINT mask) {
  SYNC_CALL(OMSetBlendState(s, factor, mask));
  auto* r = emit<BlendArgs>(Op::OMSetBlendState);
  r->s = s;
  r->has_factor = factor != nullptr;
  if (factor) std::memcpy(r->factor, factor, sizeof r->factor);
  r->mask = mask;
}
void Recorder::OMSetDepthStencilState(ID3D11DepthStencilState* s, UINT ref) {
  SYNC_CALL(OMSetDepthStencilState(s, ref));
  auto* r = emit<DepthArgs>(Op::OMSetDepthStencilState);
  r->s = s;
  r->ref = ref;
}
void Recorder::RSSetState(ID3D11RasterizerState* s) {
  SYNC_CALL(RSSetState(s));
  emit<PtrArgs>(Op::RSSetState)->p = s;
}
void Recorder::RSSetViewports(UINT n, const D3D11_VIEWPORT* vps) {
  SYNC_CALL(RSSetViewports(n, vps));
  auto* r = emit<CountArgs>(Op::RSSetViewports, n * sizeof(D3D11_VIEWPORT));
  r->n = n;
  if (n) std::memcpy(payload(r), vps, n * sizeof(D3D11_VIEWPORT));
}
void Recorder::RSSetScissorRects(UINT n, const D3D11_RECT* rects) {
  SYNC_CALL(RSSetScissorRects(n, rects));
  auto* r = emit<CountArgs>(Op::RSSetScissorRects, n * sizeof(D3D11_RECT));
  r->n = n;
  if (n) std::memcpy(payload(r), rects, n * sizeof(D3D11_RECT));
}
void Recorder::IASetInputLayout(ID3D11InputLayout* layout) {
  SYNC_CALL(IASetInputLayout(layout));
  emit<PtrArgs>(Op::IASetInputLayout)->p = layout;
}
void Recorder::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topo) {
  SYNC_CALL(IASetPrimitiveTopology(topo));
  emit<TopoArgs>(Op::IASetPrimitiveTopology)->t = topo;
}
void Recorder::IASetIndexBuffer(ID3D11Buffer* b, DXGI_FORMAT format, UINT offset) {
  SYNC_CALL(IASetIndexBuffer(b, format, offset));
  auto* r = emit<IbArgs>(Op::IASetIndexBuffer);
  r->b = b;
  r->f = format;
  r->off = offset;
}
void Recorder::VSSetShader(ID3D11VertexShader* s, ID3D11ClassInstance* const* ci, UINT nci) {
  SYNC_CALL(VSSetShader(s, ci, nci));
  emit<PtrArgs>(Op::VSSetShader)->p = s;
}
void Recorder::PSSetShader(ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, UINT nci) {
  SYNC_CALL(PSSetShader(s, ci, nci));
  emit<PtrArgs>(Op::PSSetShader)->p = s;
}

namespace {
template <class T>
void emit_views(Op op, UINT slot, UINT n, T* const* items) {
  auto* r = emit<ViewsArgs>(op, n * sizeof(void*));
  r->slot = slot;
  r->n = n;
  if (n) std::memcpy(payload(r), items, n * sizeof(void*));
}
}  // namespace

void Recorder::VSSetConstantBuffers(UINT slot, UINT n, ID3D11Buffer* const* bufs) {
  SYNC_CALL(VSSetConstantBuffers(slot, n, bufs));
  emit_views(Op::VSSetConstantBuffers, slot, n, bufs);
}
void Recorder::PSSetConstantBuffers(UINT slot, UINT n, ID3D11Buffer* const* bufs) {
  SYNC_CALL(PSSetConstantBuffers(slot, n, bufs));
  emit_views(Op::PSSetConstantBuffers, slot, n, bufs);
}
void Recorder::VSSetShaderResources(UINT slot, UINT n, ID3D11ShaderResourceView* const* views) {
  SYNC_CALL(VSSetShaderResources(slot, n, views));
  emit_views(Op::VSSetShaderResources, slot, n, views);
}
void Recorder::PSSetShaderResources(UINT slot, UINT n, ID3D11ShaderResourceView* const* views) {
  SYNC_CALL(PSSetShaderResources(slot, n, views));
  emit_views(Op::PSSetShaderResources, slot, n, views);
}
void Recorder::PSSetSamplers(UINT slot, UINT n, ID3D11SamplerState* const* samplers) {
  SYNC_CALL(PSSetSamplers(slot, n, samplers));
  emit_views(Op::PSSetSamplers, slot, n, samplers);
}
void Recorder::Draw(UINT count, UINT start) {
  SYNC_CALL(Draw(count, start));
  auto* r = emit<DrawArgs>(Op::Draw);
  r->count = count;
  r->start = start;
  r->base = 0;
}
void Recorder::DrawIndexed(UINT count, UINT start, INT base_vertex) {
  SYNC_CALL(DrawIndexed(count, start, base_vertex));
  auto* r = emit<DrawArgs>(Op::DrawIndexed);
  r->count = count;
  r->start = start;
  r->base = base_vertex;
}
void Recorder::ClearRenderTargetView(ID3D11RenderTargetView* v, const FLOAT color[4]) {
  SYNC_CALL(ClearRenderTargetView(v, color));
  auto* r = emit<ClearRtArgs>(Op::ClearRenderTargetView);
  r->v = v;
  std::memcpy(r->c, color, sizeof r->c);
}
void Recorder::ClearDepthStencilView(ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil) {
  SYNC_CALL(ClearDepthStencilView(v, flags, depth, stencil));
  auto* r = emit<ClearDsArgs>(Op::ClearDepthStencilView);
  r->v = v;
  r->flags = flags;
  r->d = depth;
  r->s = stencil;
}
void Recorder::CopyResource(ID3D11Resource* dst, ID3D11Resource* src) {
  SYNC_CALL(CopyResource(dst, src));
  auto* r = emit<CopyArgs>(Op::CopyResource);
  r->dst = dst;
  r->src = src;
}
void Recorder::UpdateSubresource(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT row_pitch,
                                 UINT depth_pitch, size_t bytes) {
  SYNC_CALL(UpdateSubresource(res, sub, box, data, row_pitch, depth_pitch));
  auto* r = emit<UpdateArgs>(Op::UpdateSubresource, bytes);
  r->r = res;
  r->sub = sub;
  r->has_box = box != nullptr;
  if (box) r->box = *box;
  r->row = row_pitch;
  r->depth = depth_pitch;
  r->bytes = bytes;
  std::memcpy(payload(r), data, bytes);
}
void Recorder::write_buffer(ID3D11Buffer* b, D3D11_MAP mode, u32 offset, const void* data, u32 bytes) {
  std::memcpy(write_buffer_space(b, mode, offset, bytes), data, bytes);
}
void* Recorder::write_buffer_space(ID3D11Buffer* b, D3D11_MAP mode, u32 offset, u32 bytes) {
  if (!g_async) {
    sync_pending();
    D3D11_MAPPED_SUBRESOURCE ms;
    if (HRESULT hr = ctx()->Map(b, 0, mode, 0, &ms); FAILED(hr)) {
      static std::atomic<int> warned{0};
      if (warned++ < 4) LOG_W("gpu", "buffer map failed (0x%08lx, mode %d): %u bytes not written", hr, int(mode), bytes);
      static std::vector<u8> sink;  // the caller still writes somewhere
      sink.resize(bytes);
      return sink.data();
    }
    g_sync_unmap = b;
    return static_cast<u8*>(ms.pData) + offset;
  }
  auto* r = emit<WriteArgs>(Op::WriteBuffer, bytes);
  r->b = b;
  r->mode = mode;
  r->offset = offset;
  r->bytes = bytes;
  return payload(r);
}
void Recorder::release(IUnknown* o) {
  if (!o) return;
  if (!g_async) {
    sync_pending();
    o->Release();
    return;
  }
  emit<PtrArgs>(Op::Release)->p = o;
}
void Recorder::call(std::function<void(ID3D11DeviceContext*)> fn) {
  if (!g_async) {
    sync_pending();
    fn(ctx());
    return;
  }
  emit<CallArgs>(Op::Call)->index = u32(g_cur->fns.size());
  g_cur->fns.push_back(std::move(fn));
}
void Recorder::call_sync(const std::function<void(ID3D11DeviceContext*)>& fn) {
  if (!g_async) {
    sync_pending();
    fn(ctx());
    return;
  }
  finish();
  fn(ctx());  // the submission thread is idle until the next flush
}

void Recorder::flush() {
  if (!g_async) {
    sync_pending();
    return;
  }
  if (!g_cur->size) return;
  auto next = take_chunk();
  {
    std::unique_lock lk(g_mu);
    g_queued_bytes += g_cur->size;
    g_queue.push_back(std::move(g_cur));
    ++g_chunks_sent;
    g_cv_work.notify_one();
    g_cv_done.wait(lk, [] { return g_queued_bytes <= kMaxQueuedBytes && g_queue.size() <= kMaxQueuedChunks; });
  }
  g_cur = std::move(next);
}

void Recorder::kick() {
  if (!g_async || !g_cur->size) return;
  if (g_idle.load(std::memory_order_relaxed) || g_cur->size > (size_t(512) << 10)) flush();
}

void Recorder::frame_end(u32 max_frames) {
  if (!g_async) {
    sync_pending();
    return;
  }
  ++g_cur->frames;
  ++g_frames_sent;
  if (!g_cur->size) call([](ID3D11DeviceContext*) {});  // a frame end is handed over even when empty
  flush();
  std::unique_lock lk(g_mu);
  g_cv_done.wait(lk, [&] { return g_frames_sent - g_frames_done.load(std::memory_order_relaxed) <= max_frames; });
}

void Recorder::finish() {
  if (!g_async) {
    sync_pending();
    return;
  }
  flush();
  std::unique_lock lk(g_mu);
  g_cv_done.wait(lk, [] { return g_chunks_done.load(std::memory_order_acquire) == g_chunks_sent; });
}

void submit_init(bool async) {
  if (!g_started) {
    g_cur = std::make_unique<Chunk>();
    g_cur->reserve(size_t(256) << 10);
    std::thread(submit_thread).detach();
    g_started = true;
  }
  g_async = async;
  LOG_I("gpu", "D3D11 calls %s", async ? "submitted on their own thread (rsx-submit)" : "made on the RSX FIFO thread");
}

void request_submit_mode(bool async) { g_want = async ? 1 : 0; }

void apply_submit_mode() {
  int want = g_want.exchange(-1);
  if (want < 0 || bool(want) == g_async) return;
  rec().finish();
  sync_pending();
  g_async = want != 0;
  LOG_I("gpu", "D3D11 calls %s", g_async ? "submitted on their own thread (rsx-submit)" : "made on the RSX FIFO thread");
}

std::string submit_memory() {
  std::lock_guard lk(g_mu);
  size_t cap = g_cur ? g_cur->cap : 0, queued = 0;
  for (auto& c : g_free) cap += c->cap;
  for (auto& c : g_queue) cap += c->cap, ++queued;
  return ac4::strprintf("%u chunks, %.0f MiB, %zu queued", g_chunks_made.load() + 1, cap / 1048576.0, queued);
}

void submit_stats(double* busy_ms, u64* cpu_cycles) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  *busy_ms = double(g_busy_ticks.load(std::memory_order_relaxed)) * 1000.0 / double(f.QuadPart);
  ULONG64 cycles = 0;
  if (HANDLE t = g_thread.load()) QueryThreadCycleTime(t, &cycles);
  *cpu_cycles = cycles;
}

}  // namespace rt::gpu
