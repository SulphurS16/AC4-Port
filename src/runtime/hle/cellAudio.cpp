// HLE: cellAudio. Ports are ring buffers of 256-sample blocks (big-endian float) in guest
// memory; every period (256 samples at 48 kHz) the mixer consumes one block per started port,
// advances the port read index, and sends CELL_AUDIO_EVENT_MIX to registered event queues.
// The period clock follows the host device's consumption (WASAPI), so guest audio stays in sync.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "common/file.h"
#include "runtime/audio/audio_out.h"
#include "runtime/config.h"
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
  u32 idle_periods = 1000;          // periods since the writer last filled a block
  u64 late = 0, missed = 0;         // blocks waited for / still unwritten after the wait (since last report)
  u64 late_total = 0, missed_total = 0;
};

std::mutex g_mu;
bool g_init = false;
u32 g_shm = 0;  // read indices (16 bytes per port) followed by port buffers
Port g_ports[kPorts];
u64 g_counter = 0;
std::vector<u64> g_keys;  // registered event queue keys

u32 index_addr(u32 port) { return g_shm + port * 16; }
u32 buffer_addr(u32 port) { return g_shm + 0x1000 + port * kMaxPortBytes; }

// Tags: a cleared block gets -0.0f (big-endian 0x80000000) in the front channels at the middle and
// the last sample. Any write by the game (silence included: +0.0f) changes them, so before mixing
// it is known whether the writer has filled the block, and whether it got to its end.
constexpr u32 kTag = 0x80000000u;
u32 tag_addr(const Port& port, u32 block, u32 i) {
  u32 s = (i & 2) ? kSamples - 1 : kSamples / 2 - 1, ch = i & 1;
  return port.addr + block * port.channels * kSamples * 4 + 4 * (s * port.channels + ch);
}
void tag_block(const Port& port, u32 block) {
  for (u32 i = 0; i < 4; ++i) ppu::w32(tag_addr(port, block, i), kTag);
}
// 0: untouched since it was cleared, 1: written in part, 2: written to its end.
int block_state(const Port& port, u32 block) {
  bool end = ppu::r32(tag_addr(port, block, 2)) != kTag || ppu::r32(tag_addr(port, block, 3)) != kTag;
  if (end) return 2;
  return ppu::r32(tag_addr(port, block, 0)) != kTag || ppu::r32(tag_addr(port, block, 1)) != kTag ? 1 : 0;
}

// Before a period is mixed: ports whose writer has been filling blocks get up to 3 ms to finish
// the block that is due (the device queue holds ~36 ms, so a short wait is inaudible; on the
// console the writer's thread would have run by now). Ports idle for a while are not waited for.
void wait_for_writers() {
  const u64 t0 = lv2::system_time_us();
  for (bool first = true;; first = false) {
    bool pending = false;
    {
      std::lock_guard lk(g_mu);
      for (auto& port : g_ports) {
        if (port.state != 2 || port.idle_periods > 32) continue;
        if (block_state(port, port.cur_pos) == 2) continue;
        pending = true;
        if (first) ++port.late, ++port.late_total;
      }
      if (!pending) return;
      if (lv2::system_time_us() - t0 > 3000) {
        for (auto& port : g_ports)
          if (port.state == 2 && port.idle_periods <= 32 && block_state(port, port.cur_pos) != 2) ++port.missed, ++port.missed_total;
        return;
      }
    }
    lv2::sleep_us(100);
  }
}

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

// The consumed block is cleared before the read index moves on (as the console's audio server
// does; RPCS3's documented behaviour): a writer that misses a period then leaves silence in the
// ring instead of a stale block that would be played again one ring later.
void advance_period(u64 now_us) {
  std::vector<u32> queues;
  {
    std::lock_guard lk(g_mu);
    for (u32 p = 0; p < kPorts; ++p) {
      Port& port = g_ports[p];
      if (port.state != 2) continue;
      port.idle_periods = block_state(port, port.cur_pos) ? 0 : port.idle_periods + 1;
      std::memset(ppu::mem() + port.addr + port.cur_pos * port.channels * kSamples * 4, 0, port.channels * kSamples * 4);
      tag_block(port, port.cur_pos);
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

// Output statistics since the last report (audio_report(), from the perf log every 5 s): the
// device queue's low point before each write (0 means the device ran dry: an audible gap), level
// and silent periods. --audio-capture also writes every mixed period to <run>/audio.f32.
struct Stats {
  u64 periods = 0, underruns = 0, silent = 0;
  u32 queue_min = ~0u;
  float peak = 0;
  double sum_sq = 0;
};
std::mutex g_stats_mu;
Stats g_stats;
u64 g_total_periods = 0, g_total_underruns = 0;

// The period clock: one period every 256/48000 s by the host's monotonic clock, as the console's
// audio server consumes blocks at an even rate. (Pacing by the device queue alone consumed periods
// in bursts at each 10 ms WASAPI engine tick, so a writer woken by one period's event had no time
// to fill the next block: stale blocks were replayed.) The device runs on its own crystal; the
// period length is trimmed by up to 0.5% toward a target queue level so the two clocks cannot
// drift apart, and the queue's cushion is refilled with silence after an underrun.
void audio_thread() {
  SetThreadDescription(GetCurrentThread(), L"cellAudio");
  bool dev = audio::out_init();
  std::vector<float> out(kSamples * 2), silence(kSamples * 2, 0.0f);
  const double period_us = double(kSamples) * 1e6 / 48000.0;
  const u32 target = 1728;   // ~36 ms queued in the device: 3.6 WASAPI engine periods of 10 ms
  const u32 low = target / 2;  // below this the cushion is rebuilt with silence
  double next = double(lv2::system_time_us());
  FILE* capture = nullptr;
  if (cfg().audio_capture) {
    std::string path = cfg().run_dir + "/audio.f32";
    try {
      ac4::guard_write(path);
      capture = std::fopen(path.c_str(), "wb");
    } catch (...) {
    }
    if (capture) LOG_I("audio", "--audio-capture: mixed output (48 kHz float stereo, raw) to %s", path.c_str());
    else LOG_W("audio", "--audio-capture: cannot create %s", path.c_str());
  }
  bool started = false;  // the device's first fill is not an underrun
  for (;;) {
    double now = double(lv2::system_time_us());
    if (now < next) {
      lv2::sleep_us(u64(next - now) + 1);
      continue;
    }
    // Fell far behind (the process was suspended or starved): resynchronise instead of mixing a
    // burst of periods back to back.
    if (now - next > 4 * period_us) next = now;
    u32 queued = 0;
    double trim = 1.0;
    if (dev) {
      queued = audio::out_queued_frames();
      if (queued < low) {
        // Underrun or low cushion: top the device queue up with silence (the game's audio clock is
        // not advanced for these).
        for (u32 q = queued; q + kSamples <= target; q += kSamples) audio::out_write(silence.data(), kSamples);
      }
      // > 0: the device consumes slower than our clock. A stalled device fills up and out_write()
      // drops periods; the game's audio clock keeps running either way.
      double err = (double(queued) - target) / double(target);
      trim = 1.0 + std::clamp(err * 0.01, -0.005, 0.005);
    }
    next += period_us * trim;
    wait_for_writers();
    mix_period(out);
    float peak = 0;
    double sq = 0;
    for (float& f : out) {
      f = std::clamp(f, -1.0f, 1.0f);
      peak = std::max(peak, std::fabs(f));
      sq += double(f) * f;
    }
    if (dev) audio::out_write(out.data(), kSamples);
    if (capture) std::fwrite(out.data(), sizeof(float), out.size(), capture);
    {
      std::lock_guard lk(g_stats_mu);
      ++g_stats.periods;
      ++g_total_periods;
      if (dev && started && queued == 0) {
        ++g_stats.underruns;
        ++g_total_underruns;
      }
      if (dev) g_stats.queue_min = std::min(g_stats.queue_min, queued);
      g_stats.peak = std::max(g_stats.peak, peak);
      g_stats.sum_sq += sq;
      if (peak < 1e-5f) ++g_stats.silent;
    }
    started = true;
    if (capture && (g_total_periods & 63) == 0) std::fflush(capture);
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
    for (u32 b = 0; b < port.blocks; ++b) tag_block(port, b);
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

// One line for the perf log: periods mixed since the last call (187.5 per second at 48 kHz), device
// underruns (the queue was empty before a write), the queue's low point, peak and RMS level of the
// mixed output and the share of silent periods.
std::string audio_report() {
  Stats s;
  {
    std::lock_guard lk(g_stats_mu);
    s = g_stats;
    g_stats = Stats{};
  }
  if (!s.periods) return "audio: no periods mixed";
  auto db = [](double v) { return v > 1e-9 ? 20.0 * std::log10(v) : -180.0; };
  double rms = std::sqrt(s.sum_sq / double(s.periods * kSamples * 2));
  std::string ports;
  {
    std::lock_guard lk(g_mu);
    for (u32 p = 0; p < kPorts; ++p) {
      Port& port = g_ports[p];
      if (port.state != 2) continue;
      ports += ac4::strprintf("; port %u: %s, late %llu, missed %llu (total %llu/%llu)", p,
                               port.idle_periods > 32 ? "idle" : "active", (unsigned long long)port.late,
                               (unsigned long long)port.missed, (unsigned long long)port.late_total,
                               (unsigned long long)port.missed_total);
      port.late = port.missed = 0;
    }
  }
  return ac4::strprintf("audio: %llu periods, underruns %llu (total %llu), device queue min %u frames, peak %.1f dBFS, rms %.1f dBFS, "
                         "silent %.0f%%%s",
                         (unsigned long long)s.periods, (unsigned long long)s.underruns, (unsigned long long)g_total_underruns,
                         s.queue_min == ~0u ? 0u : s.queue_min, db(s.peak), db(rms), 100.0 * double(s.silent) / double(s.periods),
                         ports.c_str());
}

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
