// SPU code records from the SPU code manifest (data/spu_manifest.txt): the manifest lists, for
// each record, SPU entry points and where each local-store code range lies in the user's prepared
// files (positions only, no code). This rebuilds the records (*.spurec, the format the runtime
// writes with --spu-collect) from those files, so ac4-spurecomp can compile the game's SPU code
// at preparation time instead of after training runs.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "prep/formats.h"

namespace ac4::prep {

namespace {

u32 hex32(const std::string& s) { return u32(std::stoul(s, nullptr, 16)); }

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t p = 0;
  for (;;) {
    size_t q = s.find(sep, p);
    out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  return out;
}

}  // namespace

SpuRecordStats rebuild_spu_records(const fs::path& manifest, const fs::path& prepared, const fs::path& out_dir) {
  std::ifstream in(manifest);
  if (!in) throw std::runtime_error("cannot open " + manifest.string());
  SpuRecordStats st;
  std::map<u32, std::vector<u8>> sources;  // index -> file contents (only sources whose hash matches)
  std::map<u32, std::string> source_names;
  // The manifest lists a file once per system software version it was recorded from; a file is
  // reported only when none of its versions matches.
  std::map<std::string, std::string> unmatched;  // path -> note
  std::set<std::string> matched;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string kind;
    ls >> kind;
    if (kind == "source") {
      u32 idx;
      std::string rel, sha;
      ls >> idx >> rel >> sha;
      fs::path p = prepared / fs::path(std::u8string(rel.begin(), rel.end()));
      source_names[idx] = rel;
      if (!fs::exists(p)) {
        unmatched[rel] = strprintf("source %s: missing\n", rel.c_str());
        continue;
      }
      std::string have = crypto::sha256_file_hex(p.string());
      if (have != sha) {
        if (!unmatched.count(rel))
          unmatched[rel] = strprintf("source %s: sha256 %s matches no version in the manifest (other input version)\n",
                                     rel.c_str(), have.c_str());
        continue;
      }
      matched.insert(rel);
      sources[idx] = read_file(p);
      continue;
    }
    if (kind != "record") continue;
    std::string name, entries_s;
    ls >> name >> entries_s;
    std::vector<u32> entries;
    for (const auto& e : split(entries_s, ',')) entries.push_back(hex32(e));
    std::vector<std::pair<u32, u32>> ranges;
    std::vector<u8> code;
    bool ok = true, partial = false;
    std::string r;
    while (ls >> r) {
      auto f = split(r, ':');
      if (f.size() != 4) throw std::runtime_error("malformed manifest range '" + r + "' in record " + name);
      u32 s = hex32(f[0]), e = hex32(f[1]), src = u32(std::stoul(f[2])), off = hex32(f[3]);
      auto it = sources.find(src);
      if (it == sources.end()) {  // code of the other input version (e.g. its SPURS kernel)
        partial = true;
        continue;
      }
      if (e <= s || e > 0x40000 || u64(off) + (e - s) > it->second.size()) {
        ok = false;
        break;
      }
      ranges.push_back({s, e});
      code.insert(code.end(), it->second.begin() + off, it->second.begin() + off + (e - s));
    }
    if (ok && partial) {
      // Keep the ranges this version has (typically the game's own SPU task code) and the entry
      // points inside them: their chunks are byte-identical in both versions; the other ranges'
      // code comes from that version's own records.
      std::erase_if(entries, [&](u32 x) {
        return std::none_of(ranges.begin(), ranges.end(), [&](const auto& rg) { return x >= rg.first && x < rg.second; });
      });
      name += "p";
    }
    if (!ok || ranges.empty() || entries.empty()) {
      ++st.skipped;
      continue;
    }
    if (partial) ++st.partial;
    std::vector<u8> out;
    auto put32 = [&](u32 v) {
      for (int i = 0; i < 4; ++i) out.push_back(u8(v >> (8 * i)));
    };
    out.insert(out.end(), {'S', 'P', 'U', 'R', 'E', 'C', '1', 0});
    put32(u32(entries.size()));
    put32(u32(ranges.size()));
    for (u32 e : entries) put32(e);
    for (auto [s, e] : ranges) {
      put32(s);
      put32(e);
    }
    out.insert(out.end(), code.begin(), code.end());
    fs::path dst = out_dir / (name + ".spurec");
    std::error_code ec;
    if (fs::exists(dst, ec) && read_file(dst) == out) {
      ++st.unchanged;
      continue;
    }
    write_file(dst, out);
    ++st.written;
  }
  for (const auto& [rel, note] : unmatched)
    if (!matched.count(rel)) st.notes += note;
  return st;
}

}  // namespace ac4::prep
