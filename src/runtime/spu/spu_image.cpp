// SPU images (local\research\spu_facts.md §8.2, §9.1-§9.4): sys_spu_image_import/close (liblv2)
// and the segment lists used by sys_spu_thread_initialize.
//   USER image (DIRECT import): {type 0, entry, segs -> sys_spu_segment[nsegs] (0x18 bytes each)}
//   KERNEL image (PROTECT import): {type 1, entry_point = kernel object id, segs 0, nsegs 0}
#include "runtime/spu/spu_image.h"

#include <cstring>

#include "runtime/hle.h"
#include "runtime/memory.h"

namespace rt::spu {

namespace {

struct ImageStruct {
  be32 type, entry_point, segs, nsegs;
};

struct KernelImage {
  u32 entry = 0;
  std::vector<Segment> segs;
  u32 copy = 0;  // guest copy of the ELF data
};

std::mutex g_mu;
std::map<u32, KernelImage> g_kernel;  // object id -> image
std::map<u32, u32> g_user_arrays;     // segment arrays allocated by DIRECT imports (addr -> 1)
u32 g_next_id = 0x22000001;

constexpr u32 SEG_COPY = 1, SEG_FILL = 2, SEG_INFO = 4;

// ELF program headers -> segments (§9.1). `base` is where COPY data is read from.
s32 elf_segments(u32 src, u32 base, bool with_info, u32* entry, std::vector<Segment>* out, u32* file_size) {
  if (ppu::r32(src) == 0x53434500) return s32(0x80010007);  // "SCE\0": SELF is rejected (ENOEXEC)
  if (ppu::r32(src) != 0x7F454C46) return s32(0x80010007);
  bool is64 = ppu::r8(src + 4) == 2;
  u16 machine = ppu::r16(src + 18);
  u64 e_entry = is64 ? ppu::r64(src + 24) : ppu::r32(src + 24);
  u64 phoff = is64 ? ppu::r64(src + 32) : ppu::r32(src + 28);
  u16 phentsize = ppu::r16(src + (is64 ? 54 : 42)), phnum = ppu::r16(src + (is64 ? 56 : 44));
  if (machine != 0x17 || !phnum) return s32(0x80010007);
  u32 end = 0;
  for (u16 i = 0; i < phnum; ++i) {
    u32 p = src + u32(phoff) + u32(phentsize) * i;
    u32 type = ppu::r32(p);
    u32 off, vaddr, filesz, memsz;
    if (is64) {
      off = u32(ppu::r64(p + 8));
      vaddr = u32(ppu::r64(p + 16));
      filesz = u32(ppu::r64(p + 32));
      memsz = u32(ppu::r64(p + 40));
    } else {
      off = ppu::r32(p + 4);
      vaddr = ppu::r32(p + 8);
      filesz = ppu::r32(p + 16);
      memsz = ppu::r32(p + 20);
    }
    if (type == 1) {  // PT_LOAD
      if (filesz) out->push_back({SEG_COPY, vaddr, filesz, base + off});
      if (memsz > filesz) out->push_back({SEG_FILL, vaddr + filesz, memsz - filesz, 0});
      if (!filesz && !memsz) return s32(0x80010007);  // counted but produces nothing: mismatch
    } else if (type == 4) {  // PT_NOTE
      if (with_info) out->push_back({SEG_INFO, 0, 0x20, base + off + 0x14});
    } else {
      return s32(0x80010007);
    }
    end = std::max(end, off + filesz);
  }
  *entry = u32(e_entry);
  if (file_size) *file_size = end;
  return CELL_OK;
}

}  // namespace

s32 image_import(u32 img, u32 src, u32 type) {
  if (type > 1) return CELL_EINVAL;
  if (!img || !src) return CELL_EFAULT;
  ImageStruct* im = gp<ImageStruct>(img);
  u32 entry = 0, size = 0;
  std::vector<Segment> segs;
  if (type == 1) {  // SYS_SPU_IMAGE_DIRECT: segments point into the caller's ELF
    if (s32 e = elf_segments(src, src, true, &entry, &segs, &size)) return e;
    u32 arr = vm::alloc(vm::Area::Main, u32(0x18 * std::max<size_t>(segs.size(), 1)), 0x10);
    if (!arr) return CELL_ENOMEM;
    for (size_t i = 0; i < segs.size(); ++i) {
      u32 a = arr + u32(0x18 * i);
      ppu::w32(a, segs[i].type);
      ppu::w32(a + 4, segs[i].ls);
      ppu::w32(a + 8, segs[i].size);
      ppu::w32(a + 12, 0);
      ppu::w32(a + 16, segs[i].addr);
      ppu::w32(a + 20, 0);
    }
    {
      std::lock_guard lk(g_mu);
      g_user_arrays[arr] = 1;
    }
    im->type = 0;
    im->entry_point = entry;
    im->segs = arr;
    im->nsegs = u32(segs.size());
    LOG_I("spu", "sys_spu_image_import(0x%08x, DIRECT): entry 0x%x, %zu segments", src, entry, segs.size());
    return CELL_OK;
  }
  // SYS_SPU_IMAGE_PROTECT: the kernel keeps a private copy of the ELF data.
  std::vector<Segment> probe;
  if (s32 e = elf_segments(src, src, false, &entry, &probe, &size)) return e;
  u32 copy = vm::alloc(vm::Area::Main, std::max(size, 16u), 0x10000);
  if (!copy) return CELL_ENOMEM;
  std::memcpy(ppu::mem() + copy, ppu::mem() + src, size);
  if (s32 e = elf_segments(copy, copy, false, &entry, &segs, nullptr)) return e;
  if (segs.empty() || segs.size() > 32) {
    vm::free(vm::Area::Main, copy);
    LOG_E("spu", "sys_spu_image_import(0x%08x, PROTECT): %zu segments (must be 1..32)", src, segs.size());
    return s32(0x80010007);
  }
  u32 id;
  {
    std::lock_guard lk(g_mu);
    id = g_next_id++;
    g_kernel[id] = {entry, segs, copy};
  }
  im->type = 1;
  im->entry_point = id;
  im->segs = 0;
  im->nsegs = 0;
  LOG_I("spu", "sys_spu_image_import(0x%08x, PROTECT): entry 0x%x, %zu segments -> object 0x%x", src, entry, segs.size(), id);
  return CELL_OK;
}

s32 image_close(u32 img) {
  if (!img) return CELL_EFAULT;
  ImageStruct* im = gp<ImageStruct>(img);
  std::lock_guard lk(g_mu);
  if (im->type == 0) {
    auto it = g_user_arrays.find(im->segs);
    if (it != g_user_arrays.end()) {
      vm::free(vm::Area::Main, it->first);
      g_user_arrays.erase(it);
    }
    return CELL_OK;
  }
  if (im->type == 1) {
    auto it = g_kernel.find(im->entry_point);
    if (it == g_kernel.end()) return CELL_ESRCH;
    vm::free(vm::Area::Main, it->second.copy);
    g_kernel.erase(it);
    return CELL_OK;
  }
  return CELL_EINVAL;
}

s32 image_segments(u32 img, u32* entry, std::vector<Segment>* out) {
  const ImageStruct* im = gp<ImageStruct>(img);
  if (im->type == 1) {
    std::lock_guard lk(g_mu);
    auto it = g_kernel.find(im->entry_point);
    if (it == g_kernel.end()) return CELL_ESRCH;
    *entry = it->second.entry;
    *out = it->second.segs;
    return CELL_OK;
  }
  if (im->type != 0) return CELL_EINVAL;
  u32 n = im->nsegs;
  if (im->entry_point > 0x3FFFC || n < 1 || n > 32) return CELL_EINVAL;
  bool have_copy = false, have_info = false;
  std::vector<Segment> segs;
  for (u32 i = 0; i < n; ++i) {
    u32 a = im->segs + 0x18 * i;
    Segment s{ppu::r32(a), ppu::r32(a + 4), ppu::r32(a + 8), ppu::r32(a + 16)};
    if (s.type == SEG_INFO) {
      if (s.size > 256 || have_info) return CELL_EINVAL;
      have_info = true;
      continue;
    }
    if (s.type != SEG_COPY && s.type != SEG_FILL) return CELL_EINVAL;
    if (s.type == SEG_COPY && (s.addr & 3)) return CELL_EINVAL;
    if (!s.size || (s.ls & 15) || (s.size & 15) || s.ls >= kLsSize || s.size > kLsSize) return CELL_EINVAL;
    for (const Segment& o : segs)
      if (s.ls < o.ls + o.size && o.ls < s.ls + s.size) return CELL_EINVAL;  // overlap
    have_copy |= s.type == SEG_COPY;
    segs.push_back(s);
  }
  if (!have_copy) return CELL_EINVAL;
  *entry = im->entry_point;
  *out = std::move(segs);
  return CELL_OK;
}

}  // namespace rt::spu
