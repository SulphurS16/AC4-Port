#include "launcher/download.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "launcher/process.h"

namespace launcher {
namespace {

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

// An open GET request with status 200 (or the reason it is not).
struct Request {
  Handle session, conn, req;
  unsigned long long total = 0;  // Content-Length, 0 when not sent
  std::string error;
};

void open_request(Request& r, const std::wstring& url) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = DWORD(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
    r.error = "not an https address: " + narrow(url);
    return;
  }
  r.session.h = WinHttpOpen(L"ac4-launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!r.session.h) {
    r.error = "the internet connection could not be opened";
    return;
  }
  r.conn.h = WinHttpConnect(r.session.h, host, uc.nPort, 0);
  if (!r.conn.h) {
    r.error = "cannot reach " + narrow(host);
    return;
  }
  r.req.h = WinHttpOpenRequest(r.conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!r.req.h) {
    r.error = "cannot request " + narrow(url);
    return;
  }
  if (!WinHttpSendRequest(r.req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r.req.h, nullptr)) {
    r.error = "download failed (" + narrow(host) + ", error " + std::to_string(GetLastError()) + "): check the internet connection";
    return;
  }
  DWORD status = 0, size = sizeof status;
  WinHttpQueryHeaders(r.req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    r.error = "download failed: the server answered " + std::to_string(status) + " for " + narrow(url);
    return;
  }
  wchar_t lenbuf[32] = {};
  DWORD lensize = sizeof lenbuf;
  if (WinHttpQueryHeaders(r.req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &lensize, WINHTTP_NO_HEADER_INDEX))
    r.total = _wcstoui64(lenbuf, nullptr, 10);
}

}  // namespace

std::string download(const std::wstring& url, const std::filesystem::path& to,
                     const std::function<void(unsigned long long, unsigned long long)>& progress, const std::atomic<bool>& cancel) {
  Request r;
  open_request(r, url);
  if (!r.error.empty()) return r.error;
  std::filesystem::path part = to;
  part += ".part";
  std::error_code ec;
  std::filesystem::create_directories(to.parent_path(), ec);
  FILE* f = _wfopen(part.c_str(), L"wb");
  if (!f) return "cannot write " + narrow(part.wstring());
  std::vector<char> buf(1 << 20);
  unsigned long long got = 0;
  std::string err;
  for (;;) {
    if (cancel.load()) {
      err = "cancelled";
      break;
    }
    DWORD n = 0;
    if (!WinHttpReadData(r.req.h, buf.data(), DWORD(buf.size()), &n)) {
      err = "download interrupted (error " + std::to_string(GetLastError()) + ")";
      break;
    }
    if (!n) break;
    if (std::fwrite(buf.data(), 1, n, f) != n) {
      err = "cannot write " + narrow(part.wstring()) + " (disk full?)";
      break;
    }
    got += n;
    progress(got, r.total);
  }
  std::fclose(f);
  if (err.empty() && r.total && got != r.total) err = "download incomplete";
  if (!err.empty()) {
    std::filesystem::remove(part, ec);
    return err;
  }
  std::filesystem::rename(part, to, ec);
  if (ec) return "cannot rename " + narrow(part.wstring());
  return "";
}

}  // namespace launcher
