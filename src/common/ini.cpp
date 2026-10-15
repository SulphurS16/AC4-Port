#include "common/ini.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "common/file.h"

namespace ac4 {
namespace {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

Ini Ini::parse(const std::string& text) {
  Ini ini;
  std::istringstream in(text);
  std::string raw, section;
  while (std::getline(in, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    Line l;
    l.raw = raw;
    std::string t = trim(raw);
    if (!t.empty() && t.front() == '[' && t.back() == ']') {
      section = lower(trim(t.substr(1, t.size() - 2)));
      l.is_header = true;
    } else if (!t.empty() && t[0] != '#' && t[0] != ';') {
      size_t eq = t.find('=');
      if (eq != std::string::npos) {
        l.is_key = true;
        l.key = lower(trim(t.substr(0, eq)));
        std::string v = t.substr(eq + 1);
        size_t hash = v.find(" #");  // tolerated trailing comment
        if (hash != std::string::npos) v.resize(hash);
        l.value = trim(v);
      }
    }
    l.section = section;
    ini.lines_.push_back(std::move(l));
  }
  return ini;
}

Ini Ini::load(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return Ini();
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF)
    s.erase(0, 3);  // UTF-8 byte order mark (editors add one)
  return parse(s);
}

std::string Ini::text() const {
  std::string s;
  for (const Line& l : lines_) s += l.raw + "\r\n";
  return s;
}

void Ini::save(const std::filesystem::path& path) const {
  guard_write(path);
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  write_text(tmp, text());
  if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DWORD err = GetLastError();
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    throw std::runtime_error(strprintf("cannot replace %s (error %lu)", path.string().c_str(), err));
  }
}

bool Ini::has_section(const std::string& section) const {
  std::string s = lower(section);
  if (s.empty()) return true;
  return std::any_of(lines_.begin(), lines_.end(), [&](const Line& l) { return l.is_header && l.section == s; });
}

std::optional<std::string> Ini::get(const std::string& section, const std::string& key) const {
  std::string s = lower(section), k = lower(key);
  for (auto it = lines_.rbegin(); it != lines_.rend(); ++it)  // the last occurrence wins
    if (it->is_key && it->section == s && it->key == k) return it->value;
  return std::nullopt;
}

std::string Ini::get_or(const std::string& section, const std::string& key, const std::string& fallback) const {
  auto v = get(section, key);
  return v ? *v : fallback;
}

size_t Ini::section_end(const std::string& s) const {
  // After the section's last key or header line (trailing blank/comment lines stay below it).
  size_t end = std::string::npos;
  for (size_t i = 0; i < lines_.size(); ++i)
    if (lines_[i].section == s && (lines_[i].is_key || lines_[i].is_header)) end = i + 1;
  if (end == std::string::npos && s.empty()) {  // no top-level keys: before the first header
    end = 0;
    while (end < lines_.size() && !lines_[end].is_header) ++end;
  }
  return end;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
  std::string s = lower(section), k = lower(key);
  for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
    if (!(it->is_key && it->section == s && it->key == k)) continue;
    if (it->value == value) return;
    size_t eq = it->raw.find('=');
    it->raw = it->raw.substr(0, eq + 1) + " " + value;
    it->value = value;
    return;
  }
  size_t at = section_end(s);
  if (at == std::string::npos) {
    if (!lines_.empty() && !trim(lines_.back().raw).empty()) {
      Line blank;
      blank.section = s;
      lines_.push_back(blank);
    }
    Line h;
    h.raw = "[" + section + "]";
    h.section = s;
    h.is_header = true;
    lines_.push_back(h);
    at = lines_.size();
  }
  Line l;
  l.raw = key + " = " + value;
  l.section = s;
  l.key = k;
  l.value = value;
  l.is_key = true;
  lines_.insert(lines_.begin() + std::ptrdiff_t(at), l);
}

bool Ini::remove(const std::string& section, const std::string& key) {
  std::string s = lower(section), k = lower(key);
  size_t before = lines_.size();
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(), [&](const Line& l) { return l.is_key && l.section == s && l.key == k; }),
               lines_.end());
  return lines_.size() != before;
}

std::vector<std::pair<std::string, std::string>> Ini::entries(const std::string& section) const {
  std::string s = lower(section);
  std::vector<std::pair<std::string, std::string>> out;
  for (const Line& l : lines_)
    if (l.is_key && l.section == s) out.emplace_back(l.key, l.value);
  return out;
}

void Ini::add_line(const std::string& section, const std::string& line) {
  std::string s = lower(section);
  size_t at = section_end(s);
  if (at == std::string::npos) {
    if (!lines_.empty() && !trim(lines_.back().raw).empty()) {
      Line blank;
      blank.section = s;
      lines_.push_back(blank);
    }
    Line h;
    h.raw = "[" + section + "]";
    h.section = s;
    h.is_header = true;
    lines_.push_back(h);
    at = lines_.size();
  }
  Line l;
  l.raw = line;
  l.section = s;
  lines_.insert(lines_.begin() + std::ptrdiff_t(at), l);
}

}  // namespace ac4
