// Relocation of firmware PRX modules to fixed guest addresses (done once at preparation time so
// the recompiler and the runtime both see position-dependent images).
// Output: an ELF64 image whose LOAD segments carry final addresses, relocations applied, plus a
// program header of type 0x6F000001 whose p_vaddr is the absolute module-info address.
#include <algorithm>
#include <stdexcept>

#include "prep/formats.h"

namespace ac4::prep {

std::vector<u8> relocate_prx(const std::vector<u8>& d, u32 base, std::string* log) {
  if (d.size() < 64 || rd_be32(&d[0]) != 0x7F454C46 || rd_be16(&d[16]) != 0xFFA4) throw std::runtime_error("not a PPU PRX");
  u64 phoff = rd_be64(&d[32]);
  u16 phnum = rd_be16(&d[56]);
  struct Seg {
    u32 type, flags;
    u64 off, vaddr, paddr, filesz, memsz, align;
    u32 addr = 0;
  };
  std::vector<Seg> ph(phnum);
  for (u16 i = 0; i < phnum; ++i) {
    const u8* p = &d[phoff + i * 56ull];
    ph[i] = {rd_be32(p), rd_be32(p + 4), rd_be64(p + 8), rd_be64(p + 16), rd_be64(p + 24), rd_be64(p + 32), rd_be64(p + 40),
             rd_be64(p + 48)};
  }
  // Lay out LOAD segments one after another, 64 KiB aligned (segment memory sizes may exceed the
  // next segment's p_vaddr, so the original vaddrs cannot be kept; relocations are
  // segment-relative and do not depend on them).
  std::vector<int> loads;
  u32 image_end = base;
  for (u16 i = 0; i < phnum; ++i) {
    if (ph[i].type != 1) continue;
    ph[i].addr = image_end;
    image_end = (image_end + u32(ph[i].memsz) + 0xffff) & ~0xffffu;
    loads.push_back(i);
  }
  if (loads.empty()) throw std::runtime_error("PRX has no LOAD segments");
  // Image buffer covering all segments (memsz), relocated in place.
  std::vector<u8> img(image_end - base, 0);
  for (int i : loads) {
    if (ph[i].off + ph[i].filesz > d.size()) throw std::runtime_error("PRX segment beyond file");
    std::memcpy(&img[ph[i].addr - base], &d[ph[i].off], ph[i].filesz);
  }
  auto seg_addr = [&](u32 idx) -> u32 {
    if (idx >= loads.size()) throw std::runtime_error(strprintf("relocation references segment %u", idx));
    return ph[loads[idx]].addr;
  };
  size_t nrel = 0;
  for (u16 i = 0; i < phnum; ++i) {
    if (ph[i].type != 0x700000A4) continue;
    for (u64 r = 0; r + 24 <= ph[i].filesz; r += 24) {
      const u8* e = &d[ph[i].off + r];
      u64 off = rd_be64(e);
      u8 iv = e[10], ia = e[11];
      u32 type = rd_be32(e + 12);
      u64 ptr = rd_be64(e + 16);
      u32 raddr = seg_addr(ia) + u32(off);
      u64 rdata = (iv == 0xFF ? 0 : seg_addr(iv)) + ptr;
      if (raddr < base || raddr + 8 > image_end) throw std::runtime_error("relocation target outside the image");
      u8* t = &img[raddr - base];
      switch (type) {
        case 1: wr_be32(t, u32(rdata)); break;                                      // ADDR32
        case 4: wr_be16(t, u16(rdata)); break;                                      // ADDR16_LO
        case 5: wr_be16(t, u16(rdata >> 16)); break;                                // ADDR16_HI
        case 6: wr_be16(t, u16((rdata >> 16) + ((rdata & 0x8000) ? 1 : 0))); break;  // ADDR16_HA
        case 10: {                                                                  // REL24
          u32 w = rd_be32(t);
          wr_be32(t, (w & 0xfc000003u) | (u32(rdata - raddr) & 0x03fffffcu));
          break;
        }
        case 11: {  // REL14
          u32 w = rd_be32(t);
          wr_be32(t, (w & 0xffff0003u) | (u32(rdata - raddr) & 0x0000fffcu));
          break;
        }
        case 38: wr_be64(t, rdata); break;          // ADDR64
        case 44: wr_be64(t, rdata - raddr); break;  // REL64
        case 57: {                                  // ADDR16_LO_DS
          u16 w = rd_be16(t);
          wr_be16(t, u16((w & 3) | (u16(rdata) & ~3u)));
          break;
        }
        default: throw std::runtime_error(strprintf("unsupported PRX relocation type %u", type));
      }
      ++nrel;
    }
  }
  // Module info lives in segment 0 at file offset p_paddr.
  u32 modinfo = ph[loads[0]].addr + u32(ph[loads[0]].paddr - ph[loads[0]].off);
  if (log)
    *log += strprintf("  relocated %zu entries; image 0x%08x-0x%08x; module info 0x%08x\n", nrel, base, image_end, modinfo);
  // Write the output ELF: header + phdrs (LOADs + module-info marker) + segment data.
  u32 nph = u32(loads.size()) + 1;
  u64 data_off = 0x1000;
  std::vector<u8> out(data_off + img.size(), 0);
  std::memcpy(out.data(), d.data(), 0x40);
  wr_be64(&out[24], 0);           // e_entry
  wr_be64(&out[32], 0x40);        // e_phoff
  wr_be64(&out[40], 0);           // e_shoff
  wr_be16(&out[56], u16(nph));    // e_phnum
  wr_be16(&out[60], 0);           // e_shnum
  wr_be16(&out[62], 0);
  u32 k = 0;
  for (int i : loads) {
    u8* p = &out[0x40 + 56 * k++];
    wr_be32(p, 1);
    wr_be32(p + 4, ph[i].flags);
    wr_be64(p + 8, data_off + (ph[i].addr - base));
    wr_be64(p + 16, ph[i].addr);
    wr_be64(p + 24, ph[i].addr);
    wr_be64(p + 32, ph[i].filesz);
    wr_be64(p + 40, ph[i].memsz);
    wr_be64(p + 48, 0x10000);
  }
  u8* p = &out[0x40 + 56 * k];
  wr_be32(p, 0x6F000001);
  wr_be64(p + 16, modinfo);
  wr_be64(p + 24, modinfo);
  std::memcpy(&out[data_off], img.data(), img.size());
  return out;
}

}  // namespace ac4::prep
