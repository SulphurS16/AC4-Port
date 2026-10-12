// HLE: cellHttpUtil. URI parsing is a pure string operation and is implemented fully.
#include <cctype>

#include "runtime/hle.h"

namespace rt {
namespace {

constexpr s32 CELL_HTTP_UTIL_ERROR_NO_BUFFER = s32(0x80711002);
constexpr s32 CELL_HTTP_UTIL_ERROR_NO_STRING = s32(0x80711003);
constexpr s32 CELL_HTTP_UTIL_ERROR_INSUFFICIENT = s32(0x80711004);
constexpr s32 CELL_HTTP_UTIL_ERROR_INVALID_URI = s32(0x80711005);

struct HttpUri {
  be32 scheme, hostname, username, password, path;
  be32 port;
  u8 reserved[4];
};
static_assert(sizeof(HttpUri) == 28);

// scheme "://" [user [":" password] "@"] host [":" port] [path]
s32 cellHttpUtilParseUri(Ptr<HttpUri> uri, u32 str, u32 pool, u32 size, Ptr<be32> required) {
  if (!str) return CELL_HTTP_UTIL_ERROR_NO_STRING;
  if ((!pool || !uri) && !required) return CELL_HTTP_UTIL_ERROR_NO_BUFFER;
  std::string s = read_cstr(str, 4096);
  size_t sep = s.find("://");
  if (sep == std::string::npos || sep == 0) return CELL_HTTP_UTIL_ERROR_INVALID_URI;
  std::string scheme = s.substr(0, sep), rest = s.substr(sep + 3), user, pass, host, path = "/";
  size_t slash = rest.find('/');
  std::string authority = rest.substr(0, slash);
  if (slash != std::string::npos) path = rest.substr(slash);
  size_t at = authority.rfind('@');
  if (at != std::string::npos) {
    std::string cred = authority.substr(0, at);
    authority = authority.substr(at + 1);
    size_t colon = cred.find(':');
    user = cred.substr(0, colon);
    if (colon != std::string::npos) pass = cred.substr(colon + 1);
  }
  u32 port = 0;
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
    std::string p = authority.substr(colon + 1);
    authority = authority.substr(0, colon);
    if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos)
      return CELL_HTTP_UTIL_ERROR_INVALID_URI;
    port = u32(std::stoul(p));
  } else {
    std::string lower = scheme;
    for (char& c : lower) c = char(std::tolower(u8(c)));
    port = lower == "https" ? 443 : 80;
  }
  host = authority;
  if (host.empty()) return CELL_HTTP_UTIL_ERROR_INVALID_URI;
  const std::string* parts[] = {&scheme, &host, &user, &pass, &path};
  u32 total = 0;
  for (const std::string* p : parts) total += u32(p->size()) + 1;
  if (required) *required = total;
  if (!uri || !pool) return CELL_OK;
  if (size < total) return CELL_HTTP_UTIL_ERROR_INSUFFICIENT;
  u32 cur = pool, addrs[5];
  for (int i = 0; i < 5; ++i) {
    addrs[i] = cur;
    write_cstr(cur, *parts[i], u32(parts[i]->size()) + 1);
    cur += u32(parts[i]->size()) + 1;
  }
  uri->scheme = addrs[0];
  uri->hostname = addrs[1];
  uri->username = addrs[2];
  uri->password = addrs[3];
  uri->path = addrs[4];
  uri->port = port;
  return CELL_OK;
}

}  // namespace

static hle::Module s_module("cellHttpUtil", {
    HLE_FN(cellHttpUtilParseUri),
});

}  // namespace rt
