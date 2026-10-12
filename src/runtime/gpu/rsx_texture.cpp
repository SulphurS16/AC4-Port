// RSX texture decoding. Layout and format facts: rsx_facts.md §5.3-§5.4.
//
// 8-bit and packed formats (and DXT, decompressed here) are converted to RGBA8 with the CONTROL1
// remap applied; float formats keep float precision (remap is not allowed for them on hardware).
#include "runtime/gpu/rsx_texture.h"

#include <algorithm>
#include <cstring>

namespace rt::gpu {
namespace {

u32 rd16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }
u32 rd32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
u32 log2u(u32 v) {
  u32 l = 0;
  while ((1u << l) < v) ++l;
  return l;
}

bool is_dxt(u32 f) { return f == 0x86 || f == 0x87 || f == 0x88; }
bool is_float(u32 f) { return f == 0x9A || f == 0x9B || f == 0x9C || f == 0x9F; }

// Bytes per texel (or per 4x4 block for DXT, per 2x1 pair for the B8R8_G8R8 formats).
u32 bytes_per_unit(u32 f) {
  switch (f) {
    case 0x81: return 1;
    case 0x82: case 0x83: case 0x84: case 0x8B: case 0x8F: case 0x92: case 0x93: case 0x94: case 0x97: case 0x98:
    case 0x99: case 0x9D:
      return 2;
    case 0x85: case 0x8D: case 0x8E: case 0x90: case 0x91: case 0x95: case 0x9C: case 0x9E: case 0x9F: return 4;
    case 0x86: return 8;
    case 0x87: case 0x88: return 16;
    case 0x9A: return 8;
    case 0x9B: return 16;
    default: return 0;
  }
}
// Texels covered by one unit horizontally/vertically.
u32 unit_w(u32 f) { return is_dxt(f) ? 4 : (f == 0x8D || f == 0x8E) ? 2 : 1; }
u32 unit_h(u32 f) { return is_dxt(f) ? 4 : 1; }

u32 c5(u32 v) { return (v << 3) | (v >> 2); }
u32 c6(u32 v) { return (v << 2) | (v >> 4); }
u32 c4(u32 v) { return (v << 4) | v; }

// Native view of one texel as A, R, G, B bytes (packed 0xAARRGGBB), rsx_facts.md §5.3.
u32 fetch_argb(u32 f, const u8* p) {
  switch (f) {
    case 0x81: { u32 b = p[0]; return 0xff000000u | (b << 16) | (b << 8) | b; }
    case 0x82: { u32 v = rd16(p); return ((v >> 15) ? 0xff000000u : 0) | (c5((v >> 10) & 31) << 16) | (c5((v >> 5) & 31) << 8) | c5(v & 31); }
    case 0x9D: { u32 v = rd16(p); return 0xff000000u | (c5((v >> 10) & 31) << 16) | (c5((v >> 5) & 31) << 8) | c5(v & 31); }
    case 0x83: { u32 v = rd16(p); return (c4(v >> 12) << 24) | (c4((v >> 8) & 15) << 16) | (c4((v >> 4) & 15) << 8) | c4(v & 15); }
    case 0x84: { u32 v = rd16(p); return 0xff000000u | (c5(v >> 11) << 16) | (c6((v >> 5) & 63) << 8) | c5(v & 31); }
    case 0x8F: { u32 v = rd16(p); return 0xff000000u | (c6(v >> 10) << 16) | (c5((v >> 5) & 31) << 8) | c5(v & 31); }
    case 0x97: { u32 v = rd16(p); return ((v & 1) ? 0xff000000u : 0) | (c5(v >> 11) << 16) | (c5((v >> 6) & 31) << 8) | c5((v >> 1) & 31); }
    case 0x85: return rd32(p);  // bytes A, R, G, B
    case 0x9E: return 0xff000000u | (rd32(p) & 0xffffff);
    case 0x8B: { u32 g = p[0], b = p[1]; return (g << 24) | (b << 16) | (g << 8) | b; }  // view (B, G, B, G)
    case 0x98: case 0x99: { u32 h0 = p[0], h1 = p[1]; return (h1 << 24) | (h0 << 16) | (h1 << 8) | h0; }
    case 0x94: { u32 x = p[0]; return (x << 24) | 0x00ff0000u | (x << 8) | 0xffu; }  // (1, X, 1, X), high byte
    case 0x95: { u32 c0 = p[0], c1 = p[2]; return (c1 << 24) | (c0 << 16) | (c1 << 8) | c0; }
    case 0x90: case 0x91: { u32 z = p[0]; return (z << 24) | (z << 16) | (z << 8) | z; }  // depth high byte
    case 0x92: case 0x93: { u32 z = p[0]; return (z << 24) | (z << 16) | (z << 8) | z; }
    default: return 0xffff00ffu;
  }
}

// Applies the CONTROL1 remap to a native ARGB texel and returns RGBA bytes (little-endian u32).
u32 remap_to_rgba(u32 argb, u32 remap) {
  u8 in[4] = {u8(argb >> 24), u8(argb >> 16), u8(argb >> 8), u8(argb)};  // A, R, G, B
  u8 out[4];                                                              // A, R, G, B
  for (int c = 0; c < 4; ++c) {
    u32 src = (remap >> (2 * c)) & 3, op = (remap >> (8 + 2 * c)) & 3;
    out[c] = op == 0 ? 0 : op == 1 ? 0xff : in[src];
  }
  return u32(out[1]) | (u32(out[2]) << 8) | (u32(out[3]) << 16) | (u32(out[0]) << 24);
}

u32 morton2d(u32 x, u32 y, u32 lw, u32 lh) {
  u32 off = 0, bit = 0, m = std::min(lw, lh);
  for (u32 i = 0; i < m; ++i) {
    off |= ((x >> i) & 1) << bit++;
    off |= ((y >> i) & 1) << bit++;
  }
  if (lw > lh)
    for (u32 i = m; i < lw; ++i) off |= ((x >> i) & 1) << bit++;
  else
    for (u32 i = m; i < lh; ++i) off |= ((y >> i) & 1) << bit++;
  return off;
}
u32 morton3d(u32 x, u32 y, u32 z, u32 lw, u32 lh, u32 ld) {
  u32 off = 0, bit = 0;
  for (u32 i = 0; i < std::max({lw, lh, ld}); ++i) {
    if (i < lw) off |= ((x >> i) & 1) << bit++;
    if (i < lh) off |= ((y >> i) & 1) << bit++;
    if (i < ld) off |= ((z >> i) & 1) << bit++;
  }
  return off;
}

// ---- DXT ----
void rgb565(u32 v, u8* c) {
  c[0] = u8(c5(v >> 11));
  c[1] = u8(c6((v >> 5) & 63));
  c[2] = u8(c5(v & 31));
}
// Decodes one 4x4 block to RGBA (out[16]).
void decode_dxt_block(u32 f, const u8* b, u32* out) {
  const u8* color = f == 0x86 ? b : b + 8;
  u32 c0 = color[0] | (color[1] << 8), c1 = color[2] | (color[3] << 8);
  u8 pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (f != 0x86 || c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = u8((2 * pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = u8((pal[0][i] + 2 * pal[1][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) pal[2][i] = u8((pal[0][i] + pal[1][i]) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
  }
  u32 idx = color[4] | (color[5] << 8) | (color[6] << 16) | (u32(color[7]) << 24);
  u8 alpha[16];
  if (f == 0x87) {
    for (int i = 0; i < 16; ++i) alpha[i] = u8(c4((b[i / 2] >> (4 * (i & 1))) & 15));
  } else if (f == 0x88) {
    u32 a0 = b[0], a1 = b[1];
    u8 ap[8] = {u8(a0), u8(a1)};
    if (a0 > a1)
      for (int i = 1; i < 7; ++i) ap[i + 1] = u8(((7 - i) * a0 + i * a1) / 7);
    else {
      for (int i = 1; i < 5; ++i) ap[i + 1] = u8(((5 - i) * a0 + i * a1) / 5);
      ap[6] = 0;
      ap[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= u64(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) alpha[i] = ap[(bits >> (3 * i)) & 7];
  }
  for (int i = 0; i < 16; ++i) {
    const u8* c = pal[(idx >> (2 * i)) & 3];
    u8 a = f == 0x86 ? c[3] : alpha[i];
    out[i] = u32(c[0]) | (u32(c[1]) << 8) | (u32(c[2]) << 16) | (u32(a) << 24);
  }
}

u16 bswap16v(const u8* p) { return u16((p[0] << 8) | p[1]); }

}  // namespace

TexDesc decode_texture(const u32* regs, u32 unit) {
  auto r = [&](u32 method) { return regs[method >> 2]; };
  TexDesc t;
  t.unit = unit;
  u32 base = 0x1A00 + 0x20 * unit;
  t.offset = r(base) & 0x7fffffff;
  u32 fmt = r(base + 4);
  t.location = (fmt & 3) == 2 ? 1 : 0;
  t.cubemap = (fmt >> 2) & 1;
  t.border_color = (fmt >> 3) & 1;
  t.dim = (fmt >> 4) & 15;
  u32 fb = (fmt >> 8) & 0xff;
  t.linear = fb & 0x20;
  t.unnormalized = fb & 0x40;
  t.format = fb & ~0x60u;
  t.mips = std::max<u32>((fmt >> 16) & 0xffff, 1);
  t.address = r(base + 8);
  t.control0 = r(base + 0xC);
  t.remap = r(base + 0x10);
  t.filter = r(base + 0x14);
  u32 rect = r(base + 0x18);
  t.width = rect >> 16;
  t.height = rect & 0xffff;
  t.border = r(base + 0x1C);
  u32 c3 = r(0x1840 + 4 * unit);
  t.pitch = c3 & 0xfffff;
  t.depth = t.dim == 3 ? std::max<u32>(c3 >> 20, 1) : 1;
  if (t.dim == 1) t.height = 1;
  t.enabled = (t.control0 >> 31) & 1;
  // Clamp the level count to what the size allows.
  u32 maxdim = std::max({t.width, t.height, t.depth, 1u});
  u32 max_levels = log2u(maxdim) + 1;
  if (is_dxt(t.format)) max_levels = std::max<u32>(log2u(std::max<u32>(std::min(t.width, t.height) / 4, 1)) + 1, 1);
  t.mips = std::min(t.mips, max_levels);
  return t;
}

namespace {
struct LevelLayout {
  u32 offset, w, h, d, row_pitch, bytes;
};
// Computes the memory layout of every face/level; returns total bytes.
u32 layout(const TexDesc& t, std::vector<LevelLayout>* out) {
  u32 bpu = bytes_per_unit(t.format);
  if (!bpu || !t.width || !t.height) return 0;
  bool dxt = is_dxt(t.format);
  bool swz = !t.linear && !dxt;
  u32 faces = t.cubemap ? 6 : 1;
  u32 border = (!t.border_color && !dxt) ? (swz ? 4 : 1) : 0;
  u32 off = 0;
  for (u32 face = 0; face < faces; ++face) {
    if (swz && face) off = (off + 127) & ~127u;
    for (u32 lv = 0; lv < t.mips; ++lv) {
      u32 w = std::max(t.width >> lv, 1u), h = std::max(t.height >> lv, 1u), d = std::max(t.depth >> lv, 1u);
      u32 sw = w + 2 * border, sh = h + 2 * border;
      if (swz && border) {
        sw = 1u << log2u(w + 8);
        sh = 1u << log2u(h + 8);
      }
      u32 uw = (sw + unit_w(t.format) - 1) / unit_w(t.format), uh = (sh + unit_h(t.format) - 1) / unit_h(t.format);
      u32 pitch = t.linear ? (t.pitch ? t.pitch : uw * bpu) : uw * bpu;
      u32 bytes = pitch * uh * d;
      if (out) out->push_back({off, w, h, d, pitch, bytes});
      off += bytes;
    }
  }
  return off;
}
}  // namespace

u32 texture_size(const TexDesc& t) { return layout(t, nullptr); }

bool convert_texture(const TexDesc& t, const u8* src, TexData* out) {
  std::vector<LevelLayout> levels;
  if (!layout(t, &levels)) {
    out->error = "unsupported texture format 0x" + std::to_string(t.format);
    return false;
  }
  bool dxt = is_dxt(t.format), swz = !t.linear && !dxt;
  u32 bpu = bytes_per_unit(t.format);
  u32 border = (!t.border_color && !dxt) ? (swz ? 4 : 1) : 0;
  out->width = t.width;
  out->height = t.height;
  out->depth = t.depth;
  out->mips = t.mips;
  out->faces = t.cubemap ? 6 : 1;
  bool fl = is_float(t.format);
  u32 out_bpp = fl ? (t.format == 0x9A ? 8 : 16) : 4;
  out->format = !fl ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : t.format == 0x9A ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
  for (const LevelLayout& L : levels) {
    TexData::Level o;
    o.row_pitch = L.w * out_bpp;
    o.slice_pitch = o.row_pitch * L.h;
    o.data.assign(size_t(o.slice_pitch) * L.d, 0);
    const u8* base = src + L.offset;
    u32 sw = swz && border ? (1u << log2u(L.w + 8)) : L.w + 2 * border;
    u32 sh = swz && border ? (1u << log2u(L.h + 8)) : L.h + 2 * border;
    u32 lw = log2u(sw), lh = log2u(sh), ld = log2u(L.d);
    for (u32 z = 0; z < L.d; ++z)
      for (u32 y = 0; y < L.h; ++y) {
        u8* row = o.data.data() + size_t(z) * o.slice_pitch + size_t(y) * o.row_pitch;
        if (dxt) {
          if (y % 4) continue;  // whole block rows at once
          u32 blocks_w = (L.w + 3) / 4;
          for (u32 bx = 0; bx < blocks_w; ++bx) {
            u32 texels[16];
            decode_dxt_block(t.format, base + size_t(z) * L.row_pitch * ((L.h + 3) / 4) + size_t(y / 4) * L.row_pitch + bx * bpu,
                             texels);
            for (u32 py = 0; py < 4 && y + py < L.h; ++py)
              for (u32 px = 0; px < 4 && bx * 4 + px < L.w; ++px) {
                u32 c = texels[py * 4 + px];
                u32 argb = (c & 0xff000000u) | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
                u32 v = remap_to_rgba(argb, t.remap);
                std::memcpy(o.data.data() + size_t(z) * o.slice_pitch + size_t(y + py) * o.row_pitch + 4 * (bx * 4 + px), &v, 4);
              }
          }
          continue;
        }
        for (u32 x = 0; x < L.w; ++x) {
          u32 sx = x + border, sy = y + border;
          const u8* p;
          if (swz) {
            if (bpu > 4) {  // wide texels: Morton order over 32-bit units
              u32 units = bpu / 4;
              u32 lwu = log2u(sw * units);
              u8 tmp[16];
              for (u32 k = 0; k < units; ++k)
                std::memcpy(tmp + 4 * k, base + size_t(morton2d(sx * units + k, sy, lwu, lh)) * 4, 4);
              p = tmp;
              u8* dst = row + size_t(x) * out_bpp;
              if (t.format == 0x9A)
                for (int c = 0; c < 4; ++c) { u16 h = bswap16v(p + 2 * c); std::memcpy(dst + 2 * c, &h, 2); }
              else
                for (int c = 0; c < 4; ++c) { u32 v = rd32(p + 4 * c); std::memcpy(dst + 4 * c, &v, 4); }
              continue;
            }
            u32 m = L.d > 1 ? morton3d(sx, sy, z, lw, lh, ld) : morton2d(sx, sy, lw, lh);
            p = base + size_t(m) * bpu;
          } else {
            u32 ux = sx / unit_w(t.format);
            p = base + size_t(z) * L.row_pitch * (L.h + 2 * border) + size_t(sy) * L.row_pitch + size_t(ux) * bpu;
          }
          u8* dst = row + size_t(x) * out_bpp;
          if (fl) {
            if (t.format == 0x9A) {
              for (int c = 0; c < 4; ++c) { u16 h = bswap16v(p + 2 * c); std::memcpy(dst + 2 * c, &h, 2); }
            } else if (t.format == 0x9B) {
              for (int c = 0; c < 4; ++c) { u32 v = rd32(p + 4 * c); std::memcpy(dst + 4 * c, &v, 4); }
            } else if (t.format == 0x9C) {
              u32 v = rd32(p);
              for (int c = 0; c < 4; ++c) std::memcpy(dst + 4 * c, &v, 4);
            } else {  // Y16_X16_FLOAT: (c1, c0, c1, c0) as f32
              auto h2f = [](u16 h) {
                u32 s = (h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
                if (!e) f = m ? s | ((113 - 1) << 23) | (m << 13) : s;  // denormal approximated
                else if (e == 31) f = s | 0x7f800000u | (m << 13);
                else f = s | ((e + 112) << 23) | (m << 13);
                float r;
                std::memcpy(&r, &f, 4);
                return r;
              };
              float c0 = h2f(bswap16v(p)), c1 = h2f(bswap16v(p + 2));
              float v[4] = {c1, c0, c1, c0};
              std::memcpy(dst, v, 16);
            }
            continue;
          }
          u32 argb;
          if (t.format == 0x8D || t.format == 0x8E) {  // 2x1 compressed pairs
            bool second = sx & 1;
            u32 b, rr, g;
            if (t.format == 0x8D) {  // bytes B, R0, G, R1
              b = p[0]; g = p[2]; rr = second ? p[3] : p[1];
            } else {  // bytes R0, B, R1, G
              b = p[1]; g = p[3]; rr = second ? p[2] : p[0];
            }
            argb = 0xff000000u | (rr << 16) | (g << 8) | b;
          } else {
            argb = fetch_argb(t.format, p);
          }
          u32 v = remap_to_rgba(argb, t.remap);
          std::memcpy(dst, &v, 4);
        }
      }
    out->levels.push_back(std::move(o));
  }
  return true;
}

}  // namespace rt::gpu
