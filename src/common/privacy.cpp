#include "common/privacy.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace ac4 {
namespace {

std::string env_utf8(const wchar_t* name) {
  wchar_t buf[1024];
  DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
  if (!n || n >= std::size(buf)) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, int(n), s.data(), len, nullptr, nullptr);
  return s;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct Patterns {
  std::vector<std::string> profiles;  // lower case, both slash styles
  std::string user;                   // lower case
};

const Patterns& patterns() {
  static const Patterns p = [] {
    Patterns r;
    std::string profile = env_utf8(L"USERPROFILE");
    while (!profile.empty() && (profile.back() == '\\' || profile.back() == '/')) profile.pop_back();
    if (profile.size() > 3) {
      std::string back = lower(profile), fwd = back;
      std::replace(fwd.begin(), fwd.end(), '\\', '/');
      r.profiles = {back, fwd};
    }
    r.user = lower(env_utf8(L"USERNAME"));
    if (r.user.size() < 2) r.user.clear();  // a one-letter name would blank out ordinary text
    return r;
  }();
  return p;
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'; }

}  // namespace

std::string redact_personal(const std::string& text) {
  const Patterns& p = patterns();
  if (p.profiles.empty() && p.user.empty()) return text;
  std::string low = lower(text);
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    bool done = false;
    for (const std::string& pat : p.profiles) {
      if (low.compare(i, pat.size(), pat) == 0) {
        out += "%USERPROFILE%";
        i += pat.size();
        done = true;
        break;
      }
    }
    if (done) continue;
    if (!p.user.empty() && low.compare(i, p.user.size(), p.user) == 0 && (i == 0 || !word_char(text[i - 1])) &&
        (i + p.user.size() >= text.size() || !word_char(text[i + p.user.size()]))) {
      out += "<user>";
      i += p.user.size();
      continue;
    }
    out += text[i++];
  }
  return out;
}

}  // namespace ac4
