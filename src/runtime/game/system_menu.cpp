// The SYSTEM menu's PC SETTINGS row (GAME_NOTES "SYSTEM menu layout"). AC4's SYSTEM menu (SAVE
// GAME ... REGULATIONS, QUIT GAME) has no entry the port could take over, so a row is added under
// QUIT GAME. The menu's layout is data: a DRB (FromSoftware's menu layout format: sections of
// strings, shape and control records, dialogs of objects, scene keys) inside
// bind/lang/us/menu/systemsetting.000, a BND3 holding one zlib-compressed container of the DRB and
// its textures. From the player's disc the port builds a copy (once, kept in <data>\generated)
// whose SYSTEM dialog ("SystemSelect") has four more objects: QUIT GAME's frame, button, label and
// underline one row lower, the button with ID 9050 (no AC4 menu uses it) and the label the literal
// text "PC SETTINGS" in the labels' font; the column panel is one row taller and the new objects
// fade in with QUIT GAME's scene keys. The game builds, draws and navigates the row like its own;
// choosing it opens PC SETTINGS (menu_mouse.cpp). Only the known archive is changed, the disc
// never: the copy is served in its place (vfs::replace).
#include "runtime/game/system_menu.h"

#include <zlib.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <vector>

#include "common/crypto.h"
#include "common/file.h"
#include "runtime/log.h"

namespace rt::game {
namespace {
namespace fs = std::filesystem;

// The USA disc's archive (BLUS30027, VERSION 01.01).
constexpr const char* kKnownSha256 = "9aae4e0262e38426827cd87362e271bbb46da014b0e925b73f73c57a4b66fb86";
constexpr int kVersion = 1;  // of the change below: another version builds the copy again
constexpr s16 kRow = 56;     // the rows' spacing (SAVE GAME 147, LOAD GAME 203, ...)
constexpr const char* kLabel = "PC SETTINGS";

u32 get32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void put32(u8* p, u32 v) {
  p[0] = u8(v >> 24);
  p[1] = u8(v >> 16);
  p[2] = u8(v >> 8);
  p[3] = u8(v);
}
s16 get16(const u8* p) { return s16(u16(p[0] << 8 | p[1])); }
void put16(u8* p, s16 v) {
  p[0] = u8(u16(v) >> 8);
  p[1] = u8(v);
}
void need(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

// A DRB: sections {tag (4 bytes, stored reversed: "\0RTS" is STR), size, count, 0} + data.
struct Drb {
  struct Section {
    std::string name;
    u8 tag[4];
    u32 count = 0, z = 0;
    std::vector<u8> data;
  };
  std::vector<Section> secs;

  explicit Drb(const std::vector<u8>& d) {
    size_t off = 0;
    for (;;) {
      need(off + 16 <= d.size(), "DRB: truncated");
      Section s;
      std::memcpy(s.tag, &d[off], 4);
      for (int k = 3; k >= 0; --k)
        if (s.tag[k]) s.name += char(s.tag[k]);
      u32 size = get32(&d[off + 4]);
      s.count = get32(&d[off + 8]);
      s.z = get32(&d[off + 12]);
      need(off + 16 + size <= d.size(), "DRB: section past the end");
      s.data.assign(d.begin() + off + 16, d.begin() + off + 16 + size);
      off += 16 + size;
      bool end = s.name == "END";
      secs.push_back(std::move(s));
      if (end) break;
    }
  }
  std::vector<u8> build() {
    std::vector<u8> out;
    for (Section& s : secs) {
      while (s.data.size() % 16) s.data.push_back(0);
      size_t at = out.size();
      out.resize(at + 16);
      std::memcpy(&out[at], s.tag, 4);
      put32(&out[at + 4], u32(s.data.size()));
      put32(&out[at + 8], s.count);
      put32(&out[at + 12], s.z);
      out.insert(out.end(), s.data.begin(), s.data.end());
    }
    return out;
  }
  Section& sec(const char* name) {
    for (Section& s : secs)
      if (s.name == name) return s;
    throw std::runtime_error(std::string("DRB: no section ") + name);
  }
  u32 get(const char* s, u32 o) {
    Section& x = sec(s);
    need(o + 4 <= x.data.size(), "DRB: read past a section");
    return get32(&x.data[o]);
  }
  void set(const char* s, u32 o, u32 v) {
    Section& x = sec(s);
    need(o + 4 <= x.data.size(), "DRB: write past a section");
    put32(&x.data[o], v);
  }
  // Strings: UTF-16BE, NUL-terminated, by byte offset into STR (names are ASCII).
  std::string str(u32 o) {
    std::vector<u8>& d = sec("STR").data;
    std::string s;
    for (size_t p = o; p + 1 < d.size() && (d[p] || d[p + 1]); p += 2) s += d[p] == 0 && d[p + 1] < 0x80 ? char(d[p + 1]) : '?';
    return s;
  }
  u32 add_str(const std::string& text) {
    Section& x = sec("STR");
    u32 o = u32(x.data.size());
    for (char c : text) {
      x.data.push_back(0);
      x.data.push_back(u8(c));
    }
    x.data.push_back(0);
    x.data.push_back(0);
    ++x.count;
    return o;
  }
  // A record appended to a data section (SHPR, CTPR), 4-byte aligned; its offset.
  u32 add_record(const char* s, const std::vector<u8>& rec) {
    Section& x = sec(s);
    while (x.data.size() % 4) x.data.push_back(0);
    u32 o = u32(x.data.size());
    x.data.insert(x.data.end(), rec.begin(), rec.end());
    return o;
  }
  // An entry {type, data} appended to SHAP or CTRL; its offset.
  u32 add_entry(const char* s, u32 type, u32 data) {
    Section& x = sec(s);
    u32 o = u32(x.data.size());
    x.data.resize(o + 8);
    put32(&x.data[o], type);
    put32(&x.data[o + 4], data);
    ++x.count;
    return o;
  }
  std::vector<u8> record(const char* s, u32 o, u32 n) {
    Section& x = sec(s);
    need(o + n <= x.data.size(), "DRB: record past a section");
    return {x.data.begin() + o, x.data.begin() + o + n};
  }
};

void move_rect(u8* rec, s16 dy_top, s16 dy_bottom) {
  put16(rec + 2, s16(get16(rec + 2) + dy_top));
  put16(rec + 6, s16(get16(rec + 6) + dy_bottom));
}

std::vector<u8> add_pc_settings_row(const std::vector<u8>& drb_bytes) {
  Drb drb(drb_bytes);
  // The SYSTEM dialog (DLG entries: 64 bytes, name +0, objects: count +32, DLGO offset +36).
  u32 ndlg = drb.sec("DLG").count, dlg = ~0u;
  for (u32 i = 0; i < ndlg; ++i)
    if (drb.str(drb.get("DLG", 64 * i)) == "SystemSelect") dlg = 64 * i;
  need(dlg != ~0u, "no SystemSelect dialog");
  u32 n = drb.get("DLG", dlg + 32), first = drb.get("DLG", dlg + 36);
  // Its objects (DLGO entries: 32 bytes, name +0, SHAP offset +4, CTRL offset +8) by name.
  std::map<std::string, u32> obj;
  for (u32 k = 0; k < n; ++k) obj.emplace(drb.str(drb.get("DLGO", first + 32 * k)), first + 32 * k);
  for (const char* name : {"formspr1", "formspr10", "Exit", "obj12", "piece17", "Cursor"}) need(obj.count(name), "an object is missing");
  auto shap = [&](const char* name) { return drb.get("DLGO", obj[name] + 4); };
  auto ctrl = [&](const char* name) { return drb.get("DLGO", obj[name] + 8); };
  auto shape_type = [&](const char* name) { return drb.str(drb.get("SHAP", shap(name))); };
  auto shape_rec = [&](const char* name, u32 size) { return drb.record("SHPR", drb.get("SHAP", shap(name) + 4), size); };
  auto ctrl_rec = [&](const char* name, u32 size) { return drb.record("CTPR", drb.get("CTRL", ctrl(name) + 4), size); };
  need(shape_type("formspr10") == "Dialog" && shape_type("Exit") == "Null" && shape_type("obj12") == "Text" &&
           shape_type("piece17") == "Sprite" && drb.str(drb.get("CTRL", ctrl("Exit"))) == "Button",
       "unexpected object types");

  // QUIT GAME's frame (Dialog: 20 bytes), button (Null: 8; Button control: 28, ID first), label
  // (Text: 28 bytes, then the text: an FMG ID, or for a literal text (type byte +23 = 0) its STR
  // offset) and underline (Sprite: 28), one row lower.
  std::vector<u8> frame = shape_rec("formspr10", 20), null = shape_rec("Exit", 8), label = shape_rec("obj12", 28),
                  under = shape_rec("piece17", 28), button = ctrl_rec("Exit", 28), stat = ctrl_rec("formspr10", 4);
  need(get32(&button[0]) == 9006 && label[23] == 1, "unexpected QUIT GAME records");
  for (std::vector<u8>* r : {&frame, &null, &label, &under}) move_rect(r->data(), kRow, kRow);
  put32(&button[0], kPcSettingsButton);
  label[23] = 0;  // literal text
  u32 text = drb.add_str(kLabel);
  label.resize(32);
  put32(&label[28], text);

  struct New {
    const char *name, *like;
    const std::vector<u8>& shape;
    const std::vector<u8>& control;
  };
  const New made[] = {{"formspr11", "formspr10", frame, stat}, {"PcSettings", "Exit", null, button},
                      {"obj40", "obj12", label, stat}, {"piece18", "piece17", under, stat}};
  std::vector<u8> ins;
  for (const New& m : made) {
    u32 s = drb.add_entry("SHAP", drb.get("SHAP", shap(m.like)), drb.add_record("SHPR", m.shape));
    u32 c = drb.add_entry("CTRL", drb.get("CTRL", ctrl(m.like)), drb.add_record("CTPR", m.control));
    size_t at = ins.size();
    ins.resize(at + 32);
    put32(&ins[at], drb.add_str(m.name));
    put32(&ins[at + 4], s);
    put32(&ins[at + 8], c);
  }

  // The column panel (the dialog's first two objects, a rect and its frame, and the formspr1
  // backdrop) one row taller.
  for (u32 k = 0; k < 2; ++k) {
    Drb::Section& shpr = drb.sec("SHPR");
    u32 o = drb.get("SHAP", drb.get("DLGO", first + 32 * k + 4) + 4);
    need(o + 8 <= shpr.data.size() && get16(&shpr.data[o + 6]) == 552, "unexpected column panel");
    move_rect(&shpr.data[o], 0, kRow);
  }
  {
    Drb::Section& shpr = drb.sec("SHPR");
    u32 o = drb.get("SHAP", shap("formspr1") + 4);
    need(o + 8 <= shpr.data.size(), "unexpected column backdrop");
    move_rect(&shpr.data[o], 0, kRow);
  }

  // The new objects go before the cursor (drawn under its highlight, like the other rows); the
  // dialogs after it move along (the game finds objects by name or control ID, not by place).
  u32 at = obj["Cursor"];
  Drb::Section& dlgo = drb.sec("DLGO");
  dlgo.data.insert(dlgo.data.begin() + at, ins.begin(), ins.end());
  dlgo.count += 4;
  for (u32 i = 0; i < ndlg; ++i) {
    u32 p = 64 * i;
    if (p == dlg) drb.set("DLG", p + 32, n + 4);
    else if (drb.get("DLG", p + 32) > 0 && drb.get("DLG", p + 36) >= at) drb.set("DLG", p + 36, drb.get("DLG", p + 36) + u32(ins.size()));
  }

  // Scene keys: the frame, label and underline fade in with QUIT GAME's (SCDL lists: 16 bytes,
  // name +0, count +4, SCDO offset +8; SCDO entries: 16 bytes, name first).
  u32 nl = drb.sec("SCDL").count, list = ~0u;
  for (u32 i = 0; i < nl; ++i)
    if (drb.str(drb.get("SCDL", 16 * i)) == "Start_SystemSelect") list = 16 * i;
  need(list != ~0u, "no Start_SystemSelect scene list");
  u32 ln = drb.get("SCDL", list + 4), lo = drb.get("SCDL", list + 8);
  std::map<std::string, std::vector<u8>> keys;
  for (u32 k = 0; k < ln; ++k) keys.emplace(drb.str(drb.get("SCDO", lo + 16 * k)), drb.record("SCDO", lo + 16 * k, 16));
  std::vector<u8> add;
  for (int m : {0, 2, 3}) {
    need(keys.count(made[m].like), "no scene key to copy");
    std::vector<u8> e = keys[made[m].like];
    put32(&e[0], get32(&ins[32 * size_t(m)]));  // the new object's name
    add.insert(add.end(), e.begin(), e.end());
  }
  u32 end = lo + 16 * ln;
  Drb::Section& scdo = drb.sec("SCDO");
  scdo.data.insert(scdo.data.begin() + end, add.begin(), add.end());
  scdo.count += 3;
  for (u32 i = 0; i < nl; ++i) {
    u32 p = 16 * i;
    if (p == list) drb.set("SCDL", p + 4, ln + 3);
    else if (drb.get("SCDL", p + 8) >= end) drb.set("SCDL", p + 8, drb.get("SCDL", p + 8) + u32(add.size()));
  }
  return drb.build();
}

// The archive: BND3 with one entry {flags, compressed size, data offset, id, name offset,
// uncompressed size} at 0x20, zlib data; inside, a container: count, then 80-byte entries from 80
// {name[64], 0, offset / 16, size rounded up / 16, size} (the DRB, then its TPF).
std::vector<u8> patch_archive(const std::vector<u8>& bnd) {
  need(bnd.size() > 0x38 && std::memcmp(bnd.data(), "BND3", 4) == 0 && get32(&bnd[0x10]) == 1, "not a one-entry BND3");
  u32 csize = get32(&bnd[0x24]), doff = get32(&bnd[0x28]), usize = get32(&bnd[0x34]);
  need(doff >= 0x38 && u64(doff) + csize <= bnd.size(), "BND3 entry past the end");
  std::vector<u8> inner(usize);
  uLongf got = usize;
  need(uncompress(inner.data(), &got, &bnd[doff], csize) == Z_OK && got == usize, "inflate failed");
  need(usize >= 240 && get32(&inner[0]) == 2, "unexpected container");
  struct Entry {
    u32 offset, size;
  } e[2];
  for (int i = 0; i < 2; ++i) {
    const u8* p = &inner[80 + 80 * size_t(i) + 64];
    e[i] = {get32(p + 4) * 16, get32(p + 12)};
    need(u64(e[i].offset) + e[i].size <= usize, "container entry past the end");
  }
  need(std::memcmp(&inner[e[0].offset], "\0BRD", 4) == 0, "the first entry is not a DRB");
  std::vector<u8> drb = add_pc_settings_row({inner.begin() + e[0].offset, inner.begin() + e[0].offset + e[0].size});
  std::vector<u8> out(inner.begin(), inner.begin() + 240);
  const std::vector<u8>* parts[2] = {&drb, nullptr};
  std::vector<u8> tpf(inner.begin() + e[1].offset, inner.begin() + e[1].offset + e[1].size);
  parts[1] = &tpf;
  for (int i = 0; i < 2; ++i) {
    while (out.size() % 16) out.push_back(0);
    u8* p = &out[80 + 80 * size_t(i) + 64];
    put32(p + 4, u32(out.size() / 16));
    put32(p + 8, u32((parts[i]->size() + 15) / 16));
    put32(p + 12, u32(parts[i]->size()));
    out.insert(out.end(), parts[i]->begin(), parts[i]->end());
  }
  uLongf clen = compressBound(uLong(out.size()));
  std::vector<u8> comp(clen);
  need(compress2(comp.data(), &clen, out.data(), uLong(out.size()), Z_DEFAULT_COMPRESSION) == Z_OK, "deflate failed");
  std::vector<u8> result(bnd.begin(), bnd.begin() + doff);
  put32(&result[0x24], u32(clen));
  put32(&result[0x34], u32(out.size()));
  result.insert(result.end(), comp.begin(), comp.begin() + clen);
  return result;
}

}  // namespace

std::string system_menu_archive(const std::string& disc_root, const std::string& data_dir) {
  fs::path src = fs::path(disc_root) / kSystemMenuArchive;
  fs::path dir = fs::path(data_dir) / "generated", dst = dir / "systemsetting.000", stamp = dir / "systemsetting.000.txt";
  std::string want = std::string(kKnownSha256) + " v" + std::to_string(kVersion);
  try {
    std::error_code ec;
    if (fs::is_regular_file(dst, ec) && fs::is_regular_file(stamp, ec)) {
      std::vector<u8> s = ac4::read_file(stamp);
      if (std::string(s.begin(), s.end()).rfind(want, 0) == 0) return dst.string();
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<u8> bnd = ac4::read_file(src);
    auto h = ac4::crypto::sha256(bnd.data(), bnd.size());
    if (ac4::hex(h.data(), h.size()) != kKnownSha256) {
      LOG_W("game", "SYSTEM menu: %s is not the known file; no PC SETTINGS row", kSystemMenuArchive);
      return "";
    }
    std::vector<u8> out = patch_archive(bnd);
    fs::path tmp = dst;
    tmp += ".tmp";
    ac4::write_file(tmp, out);
    ac4::guard_write(dst);
    fs::rename(tmp, dst);
    ac4::write_text(stamp, want + "\n");
    LOG_I("game", "SYSTEM menu: PC SETTINGS row added (%s, %zu bytes, %.0f ms)", dst.string().c_str(), out.size(),
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return dst.string();
  } catch (const std::exception& e) {
    LOG_W("game", "SYSTEM menu: no PC SETTINGS row (%s)", e.what());
    return "";
  }
}

}  // namespace rt::game
