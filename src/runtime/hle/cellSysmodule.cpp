// HLE: cellSysmodule. Loading a library either starts the firmware module this port runs natively
// (LLE images mapped at boot by the loader; module_start runs on the calling thread the first
// time, module_stop on the last unload) or, for libraries implemented by this runtime (HLE), only
// records state. Dependencies load first, as the firmware's table defines them.
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "runtime/hle.h"
#include "runtime/loader.h"

namespace rt {
namespace {

constexpr s32 CELL_SYSMODULE_ERROR_UNKNOWN = s32(0x80012002);
constexpr s32 CELL_SYSMODULE_ERROR_UNLOADED = s32(0x80012003);
constexpr u16 kInternalBase = 0xf000;

enum class Impl { Hle, Lle, Absent };
struct Entry {
  const char* file;    // firmware module file (diagnostics)
  Impl impl;
  const char* lle;     // module info name when run natively
  std::vector<u16> deps;
};

// Media-related part of the firmware's sysmodule table; any other id is a library this runtime
// implements (or reports as unimplemented per function) and only has its load state recorded.
const std::map<u16, Entry>& table() {
  static const std::map<u16, Entry> t = {
      {0x0005, {"(vdec)", Impl::Hle, nullptr, {0x0024, 0x0025, 0x002b}}},
      {0x0006, {"(adec)", Impl::Hle, nullptr, {0x0026, 0x0027, 0x0028, 0x0029, 0x002c}}},
      {0x0007, {"(dmux)", Impl::Hle, nullptr, {0x002a, 0x002d}}},
      {0x0008, {"libvpost.sprx", Impl::Lle, "cellVpost_Library", {}}},
      {0x000a, {"libsre.sprx", Impl::Lle, "cellSre_Library", {}}},
      {0x0012, {"libpamf.sprx", Impl::Lle, "cellPamf_Library", {}}},
      {0x0013, {"libatrac3plus.sprx", Impl::Lle, "cellAtrac_Library", {0x0028, 0x0029}}},
      {0x001d, {"libsail.sprx", Impl::Lle, "cellSail_Library", {}}},
      {0x0024, {"libsmvd2.sprx", Impl::Hle, nullptr, {0x002b}}},   // MPEG-2 core: in HLE libvdec
      {0x0025, {"libavcdec.sprx", Impl::Hle, nullptr, {0x002b}}},  // AVC core: in HLE libvdec
      {0x0026, {"(adec lpcm)", Impl::Hle, nullptr, {0x002c}}},     // LPCM is built into libadec
      {0x0027, {"libac3dec.sprx", Impl::Absent, nullptr, {0x002c}}},
      {0x0028, {"libatxdec.sprx", Impl::Lle, "cellAtracXdec_Library", {0x002c}}},
      {0x0029, {"libat3dec.sprx", Impl::Lle, "cellAtrac3dec_Library", {0x002c}}},
      {0x002a, {"libdmuxpamf.sprx", Impl::Lle, "cellDmuxPamf_Library", {0x002d}}},
      {0x002b, {"libvdec.sprx", Impl::Hle, nullptr, {}}},
      {0x002c, {"libadec.sprx", Impl::Lle, "cellAdec_Library", {}}},
      {0x002d, {"libdmux.sprx", Impl::Lle, "cellDmux_Library", {}}},
      {0xf002, {"libvdec.sprx", Impl::Hle, nullptr, {}}},
      {0xf00c, {"libdmux.sprx", Impl::Lle, "cellDmux_Library", {}}},
      {0xf014, {"libpamf.sprx", Impl::Lle, "cellPamf_Library", {}}},
      {0xf031, {"libmp4.sprx", Impl::Absent, nullptr, {}}},
      {0xf039, {"libsail_avi.sprx", Impl::Absent, nullptr, {}}},
  };
  return t;
}

std::recursive_mutex g_mu;  // held across module_start/stop like the firmware's lwmutex
std::map<u16, u32> g_count;

// Runs module_start(0, NULL). A restart after module_stop first restores the module's writable
// segments, as a fresh load from flash would.
s32 start_module(ppu::Ctx& ctx, LleModule& m) {
  if (m.pristine.empty()) {
    for (auto [va, size] : m.data_segments) m.pristine.insert(m.pristine.end(), ppu::mem() + va, ppu::mem() + va + size);
  } else {
    size_t off = 0;
    for (auto [va, size] : m.data_segments) {
      std::memcpy(ppu::mem() + va, m.pristine.data() + off, size);
      off += size;
    }
  }
  s32 r = CELL_OK;
  if (m.start_opd) r = s32(hle::call_guest(ctx, m.start_opd, {0, 0}));
  if (m.start_opd) LOG_I("sysmodule", "%s: module_start -> %d", m.name.c_str(), r);
  else LOG_I("sysmodule", "%s: started (no module_start)", m.name.c_str());
  return CELL_OK;  // the result only decides residency (SYS_PRX_NO_RESIDENT unloads)
}

void stop_module(ppu::Ctx& ctx, LleModule& m) {
  s32 r = CELL_OK;
  if (m.stop_opd) r = s32(hle::call_guest(ctx, m.stop_opd, {0, 0}));
  LOG_I("sysmodule", "%s: module_stop -> %d", m.name.c_str(), r);
}

s32 load(ppu::Ctx& ctx, u16 id) {
  std::lock_guard lk(g_mu);
  auto it = table().find(id);
  if (it == table().end()) {
    if (id >= kInternalBase) {
      LOG_W("sysmodule", "load internal module 0x%04x: not in this port's module table", id);
      return CELL_SYSMODULE_ERROR_UNKNOWN;
    }
    ++g_count[id];
    LOG_I("sysmodule", "load module 0x%04x (HLE)", id);
    return CELL_OK;
  }
  const Entry& e = it->second;
  for (u16 dep : e.deps)
    if (s32 r = load(ctx, dep); r != CELL_OK) return r;
  if (g_count[id] == 0) {
    if (e.impl == Impl::Lle) {
      LleModule* m = find_lle_module(e.lle);
      if (!m) {
        LOG_E("sysmodule", "load module 0x%04x: %s is not prepared (run tools\\pipeline.ps1)", id, e.file);
        return CELL_ENOENT;
      }
      if (m->refs++ == 0) start_module(ctx, *m);
    } else if (e.impl == Impl::Absent) {
      LOG_W("sysmodule", "load module 0x%04x: %s is not provided by this port; its functions trap if called", id, e.file);
    }
    LOG_I("sysmodule", "load module 0x%04x (%s, %s)", id, e.file, e.impl == Impl::Lle ? "native" : e.impl == Impl::Hle ? "HLE" : "absent");
  }
  ++g_count[id];
  return CELL_OK;
}

s32 unload(ppu::Ctx& ctx, u16 id) {
  std::lock_guard lk(g_mu);
  auto c = g_count.find(id);
  if (c == g_count.end() || c->second == 0) return id >= kInternalBase ? CELL_SYSMODULE_ERROR_UNKNOWN : CELL_OK;
  if (--c->second == 0) LOG_I("sysmodule", "unload module 0x%04x", id);
  auto it = table().find(id);
  if (it == table().end()) return CELL_OK;
  const Entry& e = it->second;
  if (c->second == 0 && e.impl == Impl::Lle)
    if (LleModule* m = find_lle_module(e.lle); m && m->refs && --m->refs == 0) stop_module(ctx, *m);
  for (auto d = e.deps.rbegin(); d != e.deps.rend(); ++d)
    if (s32 r = unload(ctx, *d); r != CELL_OK) return r;
  return CELL_OK;
}

s32 cellSysmoduleInitialize() { return CELL_OK; }
s32 cellSysmoduleFinalize() { return CELL_OK; }
s32 cellSysmoduleLoadModule(ppu::Ctx& ctx, u16 id) {
  if (id >= kInternalBase) return CELL_SYSMODULE_ERROR_UNKNOWN;
  return load(ctx, id);
}
s32 cellSysmoduleUnloadModule(ppu::Ctx& ctx, u16 id) {
  if (id >= kInternalBase) return CELL_SYSMODULE_ERROR_UNKNOWN;
  return unload(ctx, id);
}
s32 cellSysmoduleLoadModuleInternal(ppu::Ctx& ctx, u16 id) {
  if (id < kInternalBase || id == 0xffff) return CELL_SYSMODULE_ERROR_UNKNOWN;
  return load(ctx, id);
}
s32 cellSysmoduleUnloadModuleInternal(ppu::Ctx& ctx, u16 id) {
  if (id < kInternalBase || id == 0xffff) return CELL_SYSMODULE_ERROR_UNKNOWN;
  return unload(ctx, id);
}
s32 cellSysmoduleIsLoaded(u16 id) {
  std::lock_guard lk(g_mu);
  auto c = g_count.find(id);
  return c != g_count.end() && c->second ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}
s32 cellSysmoduleIsLoadedEx(u16 id) {
  if (id < kInternalBase || id == 0xffff) return CELL_SYSMODULE_ERROR_UNKNOWN;
  return cellSysmoduleIsLoaded(id);
}

}  // namespace

static hle::Module s_module("cellSysmodule", {
    HLE_FN(cellSysmoduleInitialize),
    HLE_FN(cellSysmoduleFinalize),
    HLE_FN(cellSysmoduleIsLoaded),
    HLE_FN(cellSysmoduleIsLoadedEx),
    HLE_FN(cellSysmoduleLoadModule),
    HLE_FN(cellSysmoduleLoadModuleInternal),
    HLE_FN(cellSysmoduleUnloadModule),
    HLE_FN(cellSysmoduleUnloadModuleInternal),
});

}  // namespace rt
