#include "recomp/image.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace recomp {

using namespace ac4;

Image::Image(std::vector<u8> elf) : d_(std::move(elf)) {
  if (d_.size() < 64 || rd_be32(&d_[0]) != 0x7F454C46 || d_[4] != 2 || d_[5] != 2)
    throw std::runtime_error("not a big-endian ELF64");
  if (rd_be16(&d_[18]) != 0x15) throw std::runtime_error("not a PPC64 ELF");
  entry_ = u32(rd_be64(&d_[24]));
  u64 phoff = rd_be64(&d_[32]), shoff = rd_be64(&d_[40]);
  u16 phnum = rd_be16(&d_[56]), shnum = rd_be16(&d_[60]);
  for (u16 i = 0; i < phnum; ++i) {
    const u8* p = &d_[phoff + i * 56ull];
    Segment s{rd_be32(p), rd_be32(p + 4), u32(rd_be64(p + 16)), u32(rd_be64(p + 32)), u32(rd_be64(p + 40)), rd_be64(p + 8)};
    segs_.push_back(s);
    if (s.type == 7) {
      tls_vaddr = s.vaddr;
      tls_filesz = s.filesz;
      tls_memsz = s.memsz;
      tls_align = u32(rd_be64(p + 48));
    }
    if (s.type == 0x60000001) proc_param = s.vaddr;
    if (s.type == 0x60000002) prx_param = s.vaddr;
    if (s.type == 0x6F000001) modinfo_ = s.vaddr;
  }
  for (u16 i = 0; i < shnum && shoff; ++i) {
    const u8* p = &d_[shoff + i * 64ull];
    Section s{rd_be32(p + 4), u32(rd_be64(p + 8)), u32(rd_be64(p + 16)), u32(rd_be64(p + 32)), rd_be64(p + 24)};
    secs_.push_back(s);
    if (s.exec() && s.alloc() && s.size) code_.push_back({s.addr, s.addr + s.size});
  }
  if (code_.empty())  // no section headers (relocated modules): executable LOAD segments
    for (const auto& s : segs_)
      if (s.type == 1 && (s.flags & 1) && s.filesz) code_.push_back({s.vaddr, s.vaddr + s.filesz});
  std::sort(code_.begin(), code_.end());

  if (prx_param) parse_stub_table(r32(prx_param + 24), r32(prx_param + 28));
  if (!modinfo_) {
    // Executable: the OPD section is the allocated data section containing the entry descriptor.
    for (const auto& s : secs_)
      if (!s.exec() && s.alloc() && entry_ >= s.addr && entry_ < s.addr + s.size) opd_ = {s.addr, s.size};
    for (u32 a = opd_.addr; a + 8 <= opd_.addr + opd_.size; a += 8) opd_entries_.push_back({r32(a), r32(a + 4)});
    return;
  }
  // Relocated PRX module: library info {attr, version, name[28], toc, exports, imports}.
  module_name_ = cstr(modinfo_ + 4, 28);
  module_toc_ = r32(modinfo_ + 32);
  u32 es = r32(modinfo_ + 36), ee = r32(modinfo_ + 40), is = r32(modinfo_ + 44), ie = r32(modinfo_ + 48);
  parse_stub_table(is, ie);
  std::set<std::pair<u32, u32>> opds;
  for (u32 a = es; a < ee;) {
    u8 sz = r8(a);
    if (sz < 0x1c) throw std::runtime_error("bad export entry");
    u16 nfunc = u16(r32(a + 4)), nvar = u16(r32(a + 8) >> 16);  // +6 num_func, +8 num_var
    u32 name_p = r32(a + 0x10), nids = r32(a + 0x14), addrs = r32(a + 0x18);
    std::string lib = name_p ? cstr(name_p) : std::string();
    for (u32 k = 0; k < nfunc; ++k) {
      u32 opd = r32(addrs + 4 * k);
      exports_.push_back({lib, r32(nids + 4 * k), opd});
      if (mapped(opd, 8) && in_code(r32(opd))) opds.insert({r32(opd), r32(opd + 4)});
    }
    for (u32 k = nfunc; k < u32(nfunc) + nvar; ++k) {
      u32 opd = r32(addrs + 4 * k);
      exports_.push_back({lib, r32(nids + 4 * k), opd});
      // System exports (module_start/stop) are descriptors too.
      if (lib.empty() && mapped(opd, 8) && in_code(r32(opd)) && r32(opd + 4) == module_toc_) opds.insert({r32(opd), r32(opd + 4)});
    }
    a += sz;
  }
  // Scan data segments for further descriptors: (code address, module TOC).
  for (const auto& s : segs_) {
    if (s.type != 1 || (s.flags & 1)) continue;
    for (u32 a = (s.vaddr + 3) & ~3u; a + 8 <= s.vaddr + s.filesz; a += 4)
      if (r32(a + 4) == module_toc_ && in_code(r32(a)) && !(r32(a) & 3)) opds.insert({r32(a), r32(a + 4)});
  }
  opd_entries_.assign(opds.begin(), opds.end());
}

void Image::parse_stub_table(u32 stub_s, u32 stub_e) {
  for (u32 a = stub_s; a < stub_e;) {
    u8 sz = r8(a);
    if (sz < 0x2c) throw std::runtime_error("bad import stub entry size");
    u16 nfunc = u16(r32(a + 4));  // +6 num_func
    std::string mod = cstr(r32(a + 0x10));
    u32 nids = r32(a + 0x14), addrs = r32(a + 0x18);
    for (u16 k = 0; k < nfunc; ++k) imports_.push_back({mod, r32(nids + 4 * k), addrs + 4 * k});
    a += sz;
  }
}

const u8* Image::ptr(u32 va, u32 len) const {
  static const u8 zeros[64] = {};
  for (const auto& s : segs_) {
    if (s.type != 1 || !s.memsz) continue;
    if (va >= s.vaddr && u64(va) + len <= u64(s.vaddr) + s.filesz) return &d_[s.offset + (va - s.vaddr)];
    // Zero-initialised (bss) part of the segment.
    if (va >= s.vaddr + s.filesz && u64(va) + len <= u64(s.vaddr) + s.memsz && len <= sizeof zeros) return zeros;
  }
  return nullptr;
}

bool Image::mapped(u32 va, u32 len) const { return ptr(va, len) != nullptr; }

u32 Image::r32(u32 va) const {
  const u8* p = ptr(va, 4);
  if (!p) throw std::runtime_error(strprintf("read of unmapped address 0x%08x", va));
  return rd_be32(p);
}

u8 Image::r8(u32 va) const {
  const u8* p = ptr(va, 1);
  if (!p) throw std::runtime_error(strprintf("read of unmapped address 0x%08x", va));
  return *p;
}

void Image::patch32(u32 va, u32 expect, u32 value) {
  const u8* p = ptr(va, 4);
  if (!p || (p >= d_.data() + d_.size() || p < d_.data()))
    throw std::runtime_error(strprintf("patch at 0x%08x: not in the file's data", va));
  if (rd_be32(p) != expect)
    throw std::runtime_error(strprintf("patch at 0x%08x: found 0x%08x, expected 0x%08x (another game version?)", va, rd_be32(p),
                                       expect));
  u8* w = &d_[size_t(p - d_.data())];
  w[0] = u8(value >> 24);
  w[1] = u8(value >> 16);
  w[2] = u8(value >> 8);
  w[3] = u8(value);
}

std::string Image::cstr(u32 va, size_t maxlen) const {
  std::string s;
  for (size_t i = 0; i < maxlen; ++i) {
    const u8* p = ptr(va + u32(i), 1);
    if (!p || !*p) break;
    s.push_back(char(*p));
  }
  return s;
}

bool Image::in_code(u32 va) const {
  for (const auto& [b, e] : code_)
    if (va >= b && va < e) return true;
  return false;
}

}  // namespace recomp
