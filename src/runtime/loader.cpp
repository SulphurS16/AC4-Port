#include "runtime/loader.h"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"

namespace rt {

using ac4::rd_be16;
using ac4::rd_be32;
using ac4::rd_be64;

namespace {

std::map<std::pair<std::string, u32>, u32> g_lle_exports;      // (library, fnid) -> OPD address
std::map<std::pair<std::string, u32>, u32> g_lle_var_exports;  // (library, vnid) -> variable address
std::vector<std::pair<u32, u32>> g_code_segments;               // executable PT_LOAD ranges of LLE modules

bool in_code_segment(u32 a) {
  for (auto [b, e] : g_code_segments)
    if (a >= b && a < e) return true;
  return false;
}

// Links one imported variable: its reference list {u32 type, u32 addr, u32 addend} (type 0 ends
// it) is patched like the matching relocations (1 ADDR32, 4 ADDR16_LO, 6 ADDR16_HA, 57 ADDR16_LO_DS).
void link_variable(const std::string& lib, u32 vnid, u32 refs, u32 value) {
  for (u32 r = refs; ppu::r32(r); r += 12) {
    u32 type = ppu::r32(r), at = ppu::r32(r + 4), v = value + ppu::r32(r + 8);
    if (type != 1 && in_code_segment(at))
      LOG_E("loader", "variable %s::%08x referenced from code at 0x%x: the recompiled code keeps the unlinked value", lib.c_str(),
            vnid, at);
    switch (type) {
      case 1: ppu::w32(at, v); break;
      case 4: ppu::w16(at, u16(v)); break;
      case 6: ppu::w16(at, u16((v >> 16) + ((v & 0x8000) ? 1 : 0))); break;
      case 57: ppu::w16(at, u16((ppu::r16(at) & 3) | (v & 0xfffc))); break;
      default: LOG_E("loader", "variable %s::%08x: unknown reference type %u at 0x%x", lib.c_str(), vnid, type, at); break;
    }
  }
}

// Binds one import stub table [begin, end): every slot receives an OPD address. With `trace`,
// imports resolved to firmware modules go through a logging forwarder (--trace-lle).
void bind_imports(u32 begin, u32 end, size_t* total, size_t* unimpl, size_t* unknown, size_t* lle, bool trace = false) {
  for (u32 a = begin; a < end;) {
    u8 sz = ppu::r8(a);
    if (sz < 0x2c) throw std::runtime_error("bad import stub entry");
    u16 nfunc = ppu::r16(a + 6);
    std::string mod = read_cstr(ppu::r32(a + 0x10));
    u32 nids = ppu::r32(a + 0x14), slots = ppu::r32(a + 0x18);
    for (u16 k = 0; k < nfunc; ++k) {
      u32 fnid = ppu::r32(nids + 4 * k);
      u32 opd = 0;
      if (find_lle_export(mod, fnid, &opd)) {
        if (trace && mod != "cellSpurs") {  // SPURS calls are too frequent to trace usefully
          HleImport imp;
          imp.module = mod;
          imp.fnid = fnid;
          hle::Resolved r;
          imp.name = hle::find(mod, fnid, r) ? r.name : ac4::strprintf("fn_%08x", fnid);
          imp.lle_opd = opd;
          opd = hle_register_import(imp);
        }
        ppu::w32(slots + 4 * k, opd);
        if (lle) ++*lle;
      } else {
        HleImport imp;
        imp.module = mod;
        imp.fnid = fnid;
        hle::Resolved r;
        if (hle::find(mod, fnid, r)) {
          imp.name = r.name;
          imp.fn = r.fn;
          if (!r.fn && unimpl) ++*unimpl;
          if (!r.fn) LOG_I("loader", "import %s::%s is not implemented (traps if called)", mod.c_str(), r.name.c_str());
        } else {
          imp.name = ac4::strprintf("unknown_%08x", fnid);
          LOG_I("loader", "import %s::%08x has no implementation (traps if called)", mod.c_str(), fnid);
          if (unknown) ++*unknown;
        }
        ppu::w32(slots + 4 * k, hle_register_import(imp));
      }
      if (total) ++*total;
    }
    u16 nvar = ppu::r16(a + 8);
    u32 vnids = ppu::r32(a + 0x1c), vrefs = ppu::r32(a + 0x20);
    for (u16 k = 0; k < nvar; ++k) {
      u32 vnid = ppu::r32(vnids + 4 * k);
      auto it = g_lle_var_exports.find({mod, vnid});
      if (it != g_lle_var_exports.end()) link_variable(mod, vnid, ppu::r32(vrefs + 4 * k), it->second);
      else LOG_I("loader", "variable import %s::%08x is not provided (stays unlinked)", mod.c_str(), vnid);
    }
    a += sz;
  }
}

}  // namespace

std::vector<LleModule> g_lle_modules;

LleModule* find_lle_module(const std::string& name) {
  for (auto& m : g_lle_modules)
    if (m.name == name) return &m;
  return nullptr;
}

bool find_lle_export(const std::string& lib, u32 fnid, u32* opd) {
  auto it = g_lle_exports.find({lib, fnid});
  if (it == g_lle_exports.end()) return false;
  *opd = it->second;
  return true;
}

std::string load_lle_module(const std::vector<u8>& d) {
  if (d.size() < 64 || rd_be32(&d[0]) != 0x7F454C46 || rd_be16(&d[16]) != 0xFFA4) throw std::runtime_error("not a relocated PRX image");
  u64 phoff = rd_be64(&d[32]);
  u16 phnum = rd_be16(&d[56]);
  u32 modinfo = 0;
  LleModule mod;
  for (u16 i = 0; i < phnum; ++i) {
    const u8* p = &d[phoff + i * 56ull];
    u32 type = rd_be32(p), flags = rd_be32(p + 4);
    u64 off = rd_be64(p + 8);
    u32 va = u32(rd_be64(p + 16)), fsz = u32(rd_be64(p + 32)), msz = u32(rd_be64(p + 40));
    if (type == 1 && msz) {
      u32 b = va & ~0xfffu, e = (va + msz + 0xfff) & ~0xfffu;
      if (!vm::alloc_fixed(vm::Area::Main, b, e - b)) throw std::runtime_error(ac4::strprintf("cannot map module segment at 0x%x", va));
      std::memcpy(ppu::mem() + va, &d[off], fsz);
      std::memset(ppu::mem() + va + fsz, 0, msz - fsz);
      if (flags & 2) mod.data_segments.push_back({va, msz});
      if (flags & 1) g_code_segments.push_back({va, va + msz});
    } else if (type == 0x6F000001) {
      modinfo = va;
    }
  }
  if (!modinfo) throw std::runtime_error("module image lacks module info marker");
  std::string name = read_cstr(modinfo + 4, 28);
  u32 es = ppu::r32(modinfo + 36), ee = ppu::r32(modinfo + 40), is = ppu::r32(modinfo + 44), ie = ppu::r32(modinfo + 48);
  mod.name = name;
  mod.imports_begin = is;
  mod.imports_end = ie;
  size_t nexp = 0;
  for (u32 a = es; a < ee;) {
    u8 sz = ppu::r8(a);
    u16 nfunc = ppu::r16(a + 6), nvar = ppu::r16(a + 8);
    u32 name_p = ppu::r32(a + 0x10), nids = ppu::r32(a + 0x14), addrs = ppu::r32(a + 0x18);
    if (name_p) {
      std::string lib = read_cstr(name_p);
      for (u16 k = 0; k < nfunc; ++k) {
        g_lle_exports[{lib, ppu::r32(nids + 4 * k)}] = ppu::r32(addrs + 4 * k);
        ++nexp;
      }
      for (u16 k = nfunc; k < nfunc + nvar; ++k) {  // variables follow the functions
        g_lle_var_exports[{lib, ppu::r32(nids + 4 * k)}] = ppu::r32(addrs + 4 * k);
        ++nexp;
      }
    } else {  // module special functions: module_start (0xbc9a0086), module_stop (0xab779874)
      for (u16 k = 0; k < nfunc; ++k) {
        u32 nid = ppu::r32(nids + 4 * k), opd = ppu::r32(addrs + 4 * k);
        if (nid == 0xbc9a0086) mod.start_opd = opd;
        if (nid == 0xab779874) mod.stop_opd = opd;
      }
    }
    a += sz ? sz : 0x1c;
  }
  g_lle_modules.push_back(mod);
  LOG_I("loader", "LLE module '%s': %zu exports%s", name.c_str(), nexp, mod.start_opd ? ", module_start" : "");
  return name;
}

void bind_lle_imports() {
  for (const LleModule& m : g_lle_modules) {
    size_t total = 0, unimpl = 0, unknown = 0, lle = 0;
    bind_imports(m.imports_begin, m.imports_end, &total, &unimpl, &unknown, &lle, cfg().trace_lle);
    LOG_I("loader", "LLE module '%s': %zu imports (LLE %zu, HLE unimplemented %zu, unknown %zu)", m.name.c_str(), total, lle,
          unimpl, unknown);
  }
}

ExecInfo load_exec(const std::vector<u8>& d) {
  if (d.size() < 64 || rd_be32(&d[0]) != 0x7F454C46 || d[4] != 2 || rd_be16(&d[18]) != 0x15)
    throw std::runtime_error("not a PPC64 ELF executable");
  ExecInfo ex;
  ex.entry_opd = u32(rd_be64(&d[24]));
  u64 phoff = rd_be64(&d[32]);
  u16 phnum = rd_be16(&d[56]);
  u32 prx_param = 0;
  for (u16 i = 0; i < phnum; ++i) {
    const u8* p = &d[phoff + i * 56ull];
    u32 type = rd_be32(p);
    u64 off = rd_be64(p + 8);
    u32 va = u32(rd_be64(p + 16)), fsz = u32(rd_be64(p + 32)), msz = u32(rd_be64(p + 40));
    if (type == 1 && msz) {
      u32 b = va & ~0xfffu, e = (va + msz + 0xfff) & ~0xfffu;
      if (!vm::alloc_fixed(vm::Area::Main, b, e - b)) throw std::runtime_error(ac4::strprintf("cannot map segment at 0x%x", va));
      if (off + fsz > d.size()) throw std::runtime_error("segment beyond file");
      std::memcpy(ppu::mem() + va, &d[off], fsz);
      std::memset(ppu::mem() + va + fsz, 0, msz - fsz);
      ex.image_end = std::max(ex.image_end, e);
      ex.segs_size += (msz + (va % 0x10000) + 0xffff) & ~0xffffu;
      LOG_I("loader", "segment 0x%08x file 0x%x mem 0x%x", va, fsz, msz);
    } else if (type == 7) {
      ex.tls_vaddr = va;
      ex.tls_filesz = fsz;
      ex.tls_memsz = msz;
    } else if (type == 0x60000001) {
      const u8* pp = &d[off];
      if (rd_be32(pp + 4) != 0x13bcc5f6) throw std::runtime_error("bad process param magic");
      ex.sdk_version = rd_be32(pp + 12);
      ex.primary_prio = s32(rd_be32(pp + 16));
      ex.primary_stack = rd_be32(pp + 20);
      ex.malloc_pagesize = rd_be32(pp + 24);
    } else if (type == 0x60000002) {
      prx_param = va;
    }
  }
  // Primary stack size encodings (SYS_PROCESS_PRIMARY_STACK_SIZE_*).
  switch (ex.primary_stack) {
    case 0x10: ex.primary_stack = 32 * 1024; break;
    case 0x20: ex.primary_stack = 64 * 1024; break;
    case 0x30: ex.primary_stack = 96 * 1024; break;
    case 0x40: ex.primary_stack = 128 * 1024; break;
    case 0x50: ex.primary_stack = 256 * 1024; break;
    case 0x60: ex.primary_stack = 512 * 1024; break;
    case 0x70: ex.primary_stack = 1024 * 1024; break;
    default: ex.primary_stack = std::clamp<u32>((ex.primary_stack + 0xfff) & ~0xfffu, 0x10000, 0x100000); break;
  }
  // Bind imports (LLE module exports take precedence over HLE).
  size_t lle = 0;
  if (prx_param)
    bind_imports(ppu::r32(prx_param + 24), ppu::r32(prx_param + 28), &ex.imports, &ex.imports_unimplemented, &ex.imports_unknown, &lle,
                 cfg().trace_lle);
  ex.imports_lle = lle;
  LOG_I("loader", "entry opd 0x%08x, sdk 0x%x, prio %d, stack 0x%x, malloc page 0x%x, TLS 0x%x/0x%x/0x%x", ex.entry_opd,
        ex.sdk_version, ex.primary_prio, ex.primary_stack, ex.malloc_pagesize, ex.tls_vaddr, ex.tls_filesz, ex.tls_memsz);
  LOG_I("loader", "imports %zu (LLE %zu, HLE unimplemented %zu, unknown %zu)", ex.imports, ex.imports_lle, ex.imports_unimplemented, ex.imports_unknown);
  return ex;
}

std::string read_cstr(u32 addr, size_t maxlen) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < maxlen; ++i) {
    char c = char(ppu::r8(addr + u32(i)));
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

void write_cstr(u32 addr, const std::string& s, size_t bufsize) {
  if (!bufsize) return;
  size_t n = std::min(s.size(), bufsize - 1);
  std::memcpy(ppu::mem() + addr, s.data(), n);
  ppu::w8(addr + u32(n), 0);
}

}  // namespace rt
