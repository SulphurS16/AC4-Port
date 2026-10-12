// HLE: cellAudio. Ports are ring buffers of 256-sample blocks (big-endian float) in guest
// memory; every period (256 samples at 48 kHz) the mixer consumes one block per started port,
// advances the port read index, and sends CELL_AUDIO_EVENT_MIX to registered event queues.
// The period clock follows the host device's consumption (WASAPI), so guest audio stays in sync.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <thread>
#include <vector>

#include "runtime/audio/audio_out.h"
#include "runtime/hle.h"
#include "runtime/lv2/lv2.h"
#include "runtime/lv2/sync.h"
#include "runtime/memory.h"

namespace rt {
namespace lv2 {
void sleep_us(u64 us);
u32 event_queue_find_by_key(u64 key);
}  // namespace lv2

namespace {

constexpr s32 ERR_ALREADY_INIT = s32(0x80310701), ERR_NOT_INIT = s32(0x80310703), ERR_PARAM = s32(0x80310704),
              ERR_PORT_FULL = s32(0x80310705), ERR_PORT_ALREADY_RUN = s32(0x80310706), ERR_PORT_NOT_OPEN = s32(0x80310707),
              ERR_PORT_NOT_RUN = s32(0x80310708), ERR_TAG_NOT_FOUND = s32(0x8031070f), ERR_EVENT_QUEUE = s32(0x8031070d);
constexpr u32 kPorts = 8, kSamples = 256, kMaxPortBytes = 8 * 32 * kSamples * 4;
constexpr u64 kPeriodUs = 5333;  // 256 / 48000 s

struct Port {
  u32 state = 0;  // 0 closed, 1 ready, 2 run
  u32 channels = 0, blocks = 0, addr = 0, size = 0;
  u64 attr = 0;
  float level = 1.0f;
  u32 cur_pos = 0;
  u64 global_counter = 0, timestamp = 0;
};

std::mutex g_mu;
bool g_init = false;
u32 g_shm = 0;  // read indices (16 bytes per port) followed by port buffers
Port g_ports[kPorts];
u64 g_counter = 0;
std::vector<u64> g_keys;  // registered event queue keys

u32 index_addr(u32 port) { return g_shm + port * 16; }
u32 buffer_addr(u32 port) { return g_shm + 0x1000 + port * kMaxPortBytes; }

void mix_period(std::vector<float>& out) {
  std::fill(out.begin(), out.end(), 0.0f);
  std::lock_guard lk(g_mu);
  for (u32 p = 0; p < kPorts; ++p) {
    Port& port = g_ports[p];
    if (port.state != 2) continue;
    u32 block = port.cur_pos;
    u32 base = port.addr + block * port.channels * kSamples * 4;
    const float lv = port.level;
    for (u32 s = 0; s < kSamples; ++s) {
      float c[8] = {};
      for (u32 ch = 0; ch < port.channels && ch < 8; ++ch) c[ch] = ppu::bitsf(ppu::r32(base + 4 * (s * port.channels + ch)));
      float l, r;
      if (port.channels == 2) {
        l = c[0];
        r = c[1];
      } else {  // 7.1 (L R C LFE Ls Rs Lb Rb) -> stereo
        const float k = 0.7071f;
        l = c[0] + k * c[2] + k * c[4] + k * c[6];
        r = c[1] + k * c[2] + k * c[5] + k * c[7];
      }
      out[2 * s] += l * lv;
      out[2 * s + 1] += r * lv;
    }
  }
}

void advance_period(u64 now_us) {
  std::vector<u32> queues;
  {
    std::lock_guard lk(g_mu);
    for (u32 p = 0; p < kPorts; ++p) {
      Port& port = g_ports[p];
      if (port.state != 2) continue;
      port.global_counter = g_counter;
      port.timestamp = now_us;
      port.cur_pos = (port.cur_pos + 1) % port.blocks;
      ppu::w64(index_addr(p), port.cur_pos);
    }
    ++g_counter;
    for (u64 k : g_keys)
      if (u32 q = lv2::event_queue_find_by_key(k)) queues.push_back(q);
  }
  for (u32 q : queues) lv2::event_queue_send(q, 0, 0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
}

void audio_thread() {
  SetThreadDescription(GetCurrentThread(), L"cellAudio");
  bool dev = audio::out_init();
  std::vector<float> out(kSamples * 2);
  u64 next = lv2::system_time_us();
  const u32 target = 3 * kSamples + 960;  // keep ~36 ms queued in the device
  for (;;) {
    if (dev) {
      while (audio::out_queued_frames() > target) lv2::sleep_us(1000);
    } else {
      u64 now = lv2::system_time_us();
      if (next > now) lv2::sleep_us(next - now);
      next += kPeriodUs;
    }
    mix_period(out);
    for (float& f : out) f = std::clamp(f, -1.0f, 1.0f);
    if (dev) audio::out_write(out.data(), kSamples);
    advance_period(lv2::system_time_us());
  }
}

s32 cellAudioInit() {
  std::lock_guard lk(g_mu);
  if (g_init) return ERR_ALREADY_INIT;
  g_shm = vm::alloc(vm::Area::Main, 0x1000 + kPorts * kMaxPortBytes, 0x10000);
  if (!g_shm) return s32(0x80310702);
  g_init = true;
  std::thread(audio_thread).detach();
  LOG_I("audio", "cellAudioInit: shared area 0x%08x", g_shm);
  return CELL_OK;
}
s32 cellAudioQuit() {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  for (auto& p : g_ports) p.state = 0;
  return CELL_OK;  // the mixer thread keeps running idle (process-lifetime)
}

struct PortParam {
  be64 nChannel, nBlock, attr;
  bef32 level;
};
struct PortConfig {
  be32 readIndexAddr, status;
  be64 nChannel, nBlock;
  be32 portSize, portAddr;
};

s32 cellAudioPortOpen(Ptr<PortParam> param, Ptr<be32> port_num) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (!param || !port_num) return ERR_PARAM;
  u64 ch = param->nChannel, bl = param->nBlock, attr = param->attr;
  if ((ch != 2 && ch != 8) || (bl != 2 && bl != 4 && bl != 8 && bl != 16 && bl != 32)) return ERR_PARAM;
  for (u32 p = 0; p < kPorts; ++p) {
    if (g_ports[p].state) continue;
    Port& port = g_ports[p];
    port = Port{};
    port.state = 1;
    port.channels = u32(ch);
    port.blocks = u32(bl);
    port.attr = attr;
    port.addr = buffer_addr(p);
    port.size = u32(ch * bl * kSamples * 4);
    port.level = (attr & 0x1000) ? float(param->level) : 1.0f;  // CELL_AUDIO_PORTATTR_INITLEVEL
    std::memset(ppu::mem() + port.addr, 0, port.size);
    ppu::w64(index_addr(p), 0);
    *port_num = p;
    LOG_I("audio", "cellAudioPortOpen: port %u, %llu ch, %llu blocks, attr 0x%llx", p, (unsigned long long)ch,
          (unsigned long long)bl, (unsigned long long)attr);
    return CELL_OK;
  }
  return ERR_PORT_FULL;
}
s32 cellAudioGetPortConfig(u32 p, Ptr<PortConfig> cfg) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts || !cfg) return ERR_PARAM;
  const Port& port = g_ports[p];
  cfg->readIndexAddr = index_addr(p);
  cfg->status = port.state == 0 ? 0x1010 : port.state;
  cfg->nChannel = port.channels;
  cfg->nBlock = port.blocks;
  cfg->portSize = port.size;
  cfg->portAddr = port.addr;
  return CELL_OK;
}
s32 cellAudioPortStart(u32 p) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts) return ERR_PARAM;
  if (g_ports[p].state == 0) return ERR_PORT_NOT_OPEN;
  if (g_ports[p].state == 2) return ERR_PORT_ALREADY_RUN;
  g_ports[p].state = 2;
  return CELL_OK;
}
s32 cellAudioPortStop(u32 p) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts) return ERR_PARAM;
  if (g_ports[p].state == 0) return ERR_PORT_NOT_OPEN;
  if (g_ports[p].state != 2) return ERR_PORT_NOT_RUN;
  g_ports[p].state = 1;
  return CELL_OK;
}
s32 cellAudioPortClose(u32 p) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts) return ERR_PARAM;
  if (g_ports[p].state == 0) return ERR_PORT_NOT_OPEN;
  g_ports[p].state = 0;
  return CELL_OK;
}
s32 cellAudioSetPortLevel(u32 p, float level) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts) return ERR_PARAM;
  if (g_ports[p].state == 0) return ERR_PORT_NOT_OPEN;
  if (level >= 0.0f) g_ports[p].level = level;
  return CELL_OK;
}
s32 cellAudioGetPortTimestamp(u32 p, u64 tag, Ptr<be64> stamp) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts) return ERR_PARAM;
  const Port& port = g_ports[p];
  if (port.state == 0) return ERR_PORT_NOT_OPEN;
  if (port.global_counter < tag) return ERR_TAG_NOT_FOUND;
  if (stamp) *stamp = port.timestamp - (port.global_counter - tag) * kPeriodUs;
  return CELL_OK;
}
s32 cellAudioGetPortBlockTag(u32 p, u64 block, Ptr<be64> tag) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (p >= kPorts) return ERR_PARAM;
  const Port& port = g_ports[p];
  if (port.state == 0) return ERR_PORT_NOT_OPEN;
  if (block >= port.blocks) return ERR_PARAM;
  if (tag) *tag = port.global_counter + block - port.cur_pos;
  return CELL_OK;
}
s32 cellAudioSetNotifyEventQueue(u64 key) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  if (!lv2::event_queue_find_by_key(key)) return ERR_EVENT_QUEUE;
  for (u64 k : g_keys)
    if (k == key) return CELL_OK;
  g_keys.push_back(key);
  LOG_I("audio", "cellAudioSetNotifyEventQueue(key 0x%llx)", (unsigned long long)key);
  return CELL_OK;
}
s32 cellAudioRemoveNotifyEventQueue(u64 key) {
  std::lock_guard lk(g_mu);
  if (!g_init) return ERR_NOT_INIT;
  for (auto it = g_keys.begin(); it != g_keys.end(); ++it)
    if (*it == key) {
      g_keys.erase(it);
      return CELL_OK;
    }
  return ERR_PARAM;
}

}  // namespace

static hle::Module s_module("cellAudio", {
    HLE_FN(cellAudioGetPortBlockTag),
    HLE_FN(cellAudioGetPortConfig),
    HLE_FN(cellAudioGetPortTimestamp),
    HLE_FN(cellAudioInit),
    HLE_FN(cellAudioPortClose),
    HLE_FN(cellAudioPortOpen),
    HLE_FN(cellAudioPortStart),
    HLE_FN(cellAudioPortStop),
    HLE_FN(cellAudioQuit),
    HLE_FN(cellAudioRemoveNotifyEventQueue),
    HLE_FN(cellAudioSetNotifyEventQueue),
    HLE_FN(cellAudioSetPortLevel),
});

}  // namespace rt
