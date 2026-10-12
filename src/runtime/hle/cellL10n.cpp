// HLE: cellL10n (the four conversions this title imports): UCS-2 <-> UTF-8 strings and UCS-2 <->
// single-byte code page characters. Result codes and length conventions follow the system
// library's documented behaviour (as reproduced by the RPCS3 reference); single-byte code pages
// are converted by Windows with the matching code page.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/hle.h"

namespace rt {
namespace {

enum : s32 { ConversionOK = 0, SRCIllegal = 1, DSTExhausted = 2, ConverterUnknown = 3 };

// L10N code page index -> Windows code page (0: not a single/multi-byte code page).
u32 windows_code_page(u32 code) {
  static const u32 cp[] = {
      0,     0,     0,     0,     0,                                          // UTF8, UTF16, UTF32, UCS2, UCS4
      28591, 28592, 28593, 28594, 28595, 28596, 28597, 28598, 28599, 28600,  // ISO-8859-1..10
      28601, 28603, 28604, 28605, 28606,                                      // ISO-8859-11, 13..16
      437,   850,   863,   866,   932,   936,   949,   950,   1251,  1252,   // CP437 .. CP1252
      51936, 51932, 51949, 50222, 20420, 52936, 54936, 932,                  // EUC-CN .. RIS-506
      852,   1250,  737,   1253,  857,   1254,  775,   1257,  855,   858,   860, 861, 865, 869,
  };
  return code < std::size(cp) ? cp[code] : 0;
}

// One UCS-2 unit to UTF-8 (1-3 bytes); 0 for a surrogate.
int ucs2_to_utf8(u16 c, u8 out[3]) {
  if ((c & 0xf800) == 0xd800) return 0;
  if (c < 0x80) {
    out[0] = u8(c);
    return 1;
  }
  if (c < 0x800) {
    out[0] = u8(0xc0 | (c >> 6));
    out[1] = u8(0x80 | (c & 0x3f));
    return 2;
  }
  out[0] = u8(0xe0 | (c >> 12));
  out[1] = u8(0x80 | ((c >> 6) & 0x3f));
  out[2] = u8(0x80 | (c & 0x3f));
  return 3;
}

// One UTF-8 sequence (up to 3 bytes, BMP only) to UCS-2; returns its length or 0 if illegal.
int utf8_to_ucs2(const u8* s, u16* out) {
  if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
    u32 c = u32(s[0] & 0x0f) << 12 | u32(s[1] & 0x3f) << 6 | (s[2] & 0x3f);
    if (c < 0x800 || (c & 0xf800) == 0xd800) return 0;
    *out = u16(c);
    return 3;
  }
  if ((s[0] & 0xe0) == 0xc0 && s[0] > 0xc1 && (s[1] & 0xc0) == 0x80) {
    *out = u16((s[0] & 0x1f) << 6 | (s[1] & 0x3f));
    return 2;
  }
  if (s[0] & 0x80) return 0;
  *out = s[0];
  return 1;
}

s32 UCS2stoUTF8s(Ptr<be16> src, Ptr<be32> src_len, Ptr<u8> dst, Ptr<be32> dst_len) {
  if (*src_len == 0u) {
    *dst_len = 0;
    return ConversionOK;
  }
  u32 n = *src_len, len = 0, pos = 0;
  for (u32 i = 0; i < n; ++i) {
    u8 tmp[3];
    int k = ucs2_to_utf8(src[i], tmp);
    if (!k) {
      *src_len = n - i;
      *dst_len = len;
      return SRCIllegal;
    }
    len += k;
    if (dst) {
      if (*dst_len < len) {
        *src_len = n - i;
        *dst_len = len - k;
        return DSTExhausted;
      }
      for (int j = 0; j < k; ++j) dst[pos++] = tmp[j];
    }
  }
  *dst_len = len;
  return ConversionOK;
}

s32 UTF8stoUCS2s(Ptr<u8> src, Ptr<be32> src_len, Ptr<be16> dst, Ptr<be32> dst_len) {
  if (*src_len == 0u) {
    *dst_len = 0;
    return ConversionOK;
  }
  u32 n = *src_len, len = 0;
  for (u32 i = 0; i < n;) {
    u8 s[3] = {src[i], i + 1 < n ? src[i + 1] : u8(0), i + 2 < n ? src[i + 2] : u8(0)};
    u16 c = 0;
    int k = utf8_to_ucs2(s, &c);
    if (!k) {
      *src_len = n - i;
      *dst_len = len;
      return SRCIllegal;
    }
    if (dst) {
      if (*dst_len < len + 1) {
        *src_len = n - i;
        *dst_len = len;
        return DSTExhausted;
      }
      dst[len] = c;
    }
    ++len;
    i += u32(k);
  }
  *dst_len = len;
  return ConversionOK;
}

// Returns 1 (converted), 0 (no such character in the code page) or -1 (unknown code page).
s32 UCS2toSBCS(u16 src, Ptr<u8> dst, u32 code_page) {
  u32 cp = windows_code_page(code_page);
  if (!cp) return -1;
  if (src >= 0xfffe) return 0;
  if (src < 0x80) {
    *dst = u8(src);
    return 1;
  }
  wchar_t w = wchar_t(src);
  char out[4];
  BOOL used_default = FALSE;
  int k = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, &w, 1, out, sizeof out, nullptr, &used_default);
  if (k != 1 || used_default) return 0;
  *dst = u8(out[0]);
  return 1;
}

s32 SBCStoUCS2(u8 src, Ptr<be16> dst, u32 code_page) {
  u32 cp = windows_code_page(code_page);
  if (!cp) return -1;
  if (src < 0x80) {
    *dst = src;
    return 1;
  }
  char in = char(src);
  wchar_t w = 0;
  if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, &in, 1, &w, 1) != 1) return 0;
  *dst = u16(w);
  return 1;
}

}  // namespace

static hle::Module s_module("cellL10n", {
    HLE_FN(SBCStoUCS2),
    HLE_FN(UCS2stoUTF8s),
    HLE_FN(UCS2toSBCS),
    HLE_FN(UTF8stoUCS2s),
});

}  // namespace rt
