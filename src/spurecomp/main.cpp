// ac4-spurecomp: SPU → C++ recompiler.
//
//   ac4-spurecomp <out_dir> <input>...
//
// Inputs are SPU code records (*.spurec, written by the runtime with --spu-collect) or
// directories containing them. Every entry point in a record is rediscovered as a chunk (the
// same algorithm the runtime uses), deduplicated by its code hash, and emitted as a C++ function
// built from the shared instruction semantics (spu/spu_ops.h). The output is game-derived and
// stays in local/ (never distributed).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/file.h"
#include "spu/spu_isa.h"

namespace fs = std::filesystem;
using namespace spu;

namespace {

struct Image {
  std::unique_ptr<u8[]> ls{new u8[kLsSize]()};
  std::vector<u32> entries;
};

bool load_record(const fs::path& p, Image& img) {
  std::vector<u8> d = ac4::read_file(p.string());
  auto rd = [&](size_t o) { return u32(d[o]) | (u32(d[o + 1]) << 8) | (u32(d[o + 2]) << 16) | (u32(d[o + 3]) << 24); };
  if (d.size() < 16 || std::memcmp(d.data(), "SPUREC1", 8) != 0) return false;
  u32 ne = rd(8), nr = rd(12);
  size_t o = 16;
  if (d.size() < o + 4ull * ne + 8ull * nr) return false;
  for (u32 i = 0; i < ne; ++i, o += 4) img.entries.push_back(rd(o));
  std::vector<std::pair<u32, u32>> ranges;
  for (u32 i = 0; i < nr; ++i, o += 8) ranges.push_back({rd(o), rd(o + 4)});
  for (auto [s, e] : ranges) {
    if (e > kLsSize || s >= e || o + (e - s) > d.size()) return false;
    std::memcpy(img.ls.get() + s, d.data() + o, e - s);
    o += e - s;
  }
  return true;
}

struct Compiled {
  u64 hash;
  Chunk chunk;
  std::vector<u32> words;  // instruction words of all ranges (BE values), in order
};

std::string label(u32 pc) { return ac4::strprintf("L_%05x", pc); }

bool in_chunk(const Chunk& c, u32 pc) {
  for (auto [s, e] : c.ranges)
    if (pc >= s && pc < e) return true;
  return false;
}

// Code that transfers control to `target` (goto inside the chunk, exit otherwise).
std::string branch_to(const Chunk& c, u32 pc, u32 target) {
  if (!in_chunk(c, target)) return ac4::strprintf("SPU_EXIT(0x%05xu)", target);
  if (target <= pc) return ac4::strprintf("SPU_BACKEDGE(0x%05xu, %s)", target, label(target).c_str());
  return "goto " + label(target) + ";";
}

std::string emit_insn(const Chunk& c, u32 pc, u32 w) {
  Op op = decode(w);
  std::string fields = ac4::strprintf("SPU_FIELDS(0x%05xu, 0x%08xu) ", pc, w);
  std::string body;
  switch (op) {
    case Op::BR:
    case Op::BRA:
      body = branch_to(c, pc, branch_target(op, w, pc));
      break;
    case Op::BRSL:
    case Op::BRASL:
      body = ac4::strprintf("g[rt] = ::spu::ops::scalar(pc + 4); SPU_EXIT(0x%05xu)", branch_target(op, w, pc));
      break;
    case Op::BRZ:
      body = "if (SPU_PREF(rt) == 0) " + branch_to(c, pc, branch_target(op, w, pc));
      break;
    case Op::BRNZ:
      body = "if (SPU_PREF(rt) != 0) " + branch_to(c, pc, branch_target(op, w, pc));
      break;
    case Op::BRHZ:
      body = "if ((SPU_PREF(rt) & 0xffff) == 0) " + branch_to(c, pc, branch_target(op, w, pc));
      break;
    case Op::BRHNZ:
      body = "if ((SPU_PREF(rt) & 0xffff) != 0) " + branch_to(c, pc, branch_target(op, w, pc));
      break;
    case Op::BI:
    case Op::IRET:
    case Op::BISL:
    case Op::BISLED:
    case Op::BIZ:
    case Op::BINZ:
    case Op::BIHZ:
    case Op::BIHNZ: {
      std::string cond = "true";
      if (op == Op::BISLED) cond = "R::event_pending(t)";
      if (op == Op::BIZ) cond = "SPU_PREF(rt) == 0";
      if (op == Op::BINZ) cond = "SPU_PREF(rt) != 0";
      if (op == Op::BIHZ) cond = "(SPU_PREF(rt) & 0xffff) == 0";
      if (op == Op::BIHNZ) cond = "(SPU_PREF(rt) & 0xffff) != 0";
      std::string target = op == Op::IRET ? "t.srr0" : "SPU_PREF(ra)";
      u32 de = (w >> 18) & 3;
      body = "{ const bool take_ = " + cond + "; const u32 target_ = " + target + " & 0x3fffc; ";
      if (op == Op::BISL || op == Op::BISLED) body += "g[rt] = ::spu::ops::scalar(pc + 4); ";
      if (de == 1) body += "t.int_enabled = true; ";
      if (de == 2) body += "t.int_enabled = false; ";
      std::string next = de == 1 ? "R::check_interrupt(t, take_ ? target_ : pc + 4)" : "(take_ ? target_ : pc + 4)";
      bool cond_op = op == Op::BIZ || op == Op::BINZ || op == Op::BIHZ || op == Op::BIHNZ;
      if (cond_op && de != 1)
        body += "if (take_) SPU_EXIT(target_) }";  // not taken: fall through inside the chunk
      else
        body += "SPU_EXIT(" + next + ") }";
      break;
    }
    case Op::RDCH:
      body = "t.pc = pc; g[rt] = ::spu::ops::scalar(R::read_channel(t, ra));";
      break;
    case Op::RCHCNT:
      body = "t.pc = pc; g[rt] = ::spu::ops::scalar(R::channel_count(t, ra));";
      break;
    case Op::WRCH:
      body =
          "t.pc = pc; R::write_channel(t, ra, SPU_PREF(rt)); "
          "if (t.int_enabled) { const u32 n_ = R::check_interrupt(t, pc + 4); if (n_ != pc + 4) SPU_EXIT(n_) }";
      break;
    case Op::STOP:
    case Op::STOPD:
      body = "t.pc = pc; if (!R::stop_and_signal(t, w & 0x3fff)) return false; if (t.pc != pc + 4) return true;";
      break;
    case Op::UNK:
      body = "SPU_EXIT(pc)";  // never part of a chunk; the interpreter reports it
      break;
    default:
      body = std::string("SPU_OP_") + op_name(op) + ";";
      break;
  }
  return label(pc) + ": { " + fields + body + " }\n";
}

std::string emit_chunk(const Compiled& cc) {
  const Chunk& c = cc.chunk;
  std::string s = ac4::strprintf("bool spu_c_%016llx(R::Thread& t) {\n  ppu::v128* g = t.gpr;\n  goto %s;\n",
                                  (unsigned long long)cc.hash, label(c.entry).c_str());
  size_t wi = 0;
  for (auto [b, e] : c.ranges) {
    for (u32 pc = b; pc < e; pc += 4) s += "  " + emit_insn(c, pc, cc.words[wi++]);
    s += ac4::strprintf("  SPU_EXIT(0x%05xu)\n", e);  // fell off the end of a range
  }
  s += "}\n\n";
  return s;
}

const char* kPrologue =
    "// Generated by ac4-spurecomp from SPU code collected at run time. Game-derived: local only.\n"
    "#include \"spu/spu_gen.h\"\n"
    "#include \"spu/spu_ops.h\"\n\n"
    "namespace R = ::rt::spu;\n\n";

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: ac4-spurecomp <out_dir> <record-or-dir>...\n");
    return 2;
  }
  fs::path out = argv[1];
  std::vector<fs::path> files;
  for (int i = 2; i < argc; ++i) {
    fs::path p = argv[i];
    if (fs::is_directory(p)) {
      for (const auto& e : fs::directory_iterator(p))
        if (e.path().extension() == ".spurec") files.push_back(e.path());
    } else {
      files.push_back(p);
    }
  }
  std::sort(files.begin(), files.end());
  std::map<u64, Compiled> chunks;
  size_t records = 0;
  for (const auto& f : files) {
    Image img;
    if (!load_record(f, img)) {
      std::fprintf(stderr, "skipping malformed record %s\n", f.string().c_str());
      continue;
    }
    ++records;
    for (u32 entry : img.entries) {
      Chunk c = discover_chunk(img.ls.get(), entry);
      if (!c.insns) continue;
      u64 h = chunk_hash(img.ls.get(), c);
      if (chunks.count(h)) continue;
      Compiled cc{h, c, {}};
      for (auto [b, e] : c.ranges)
        for (u32 pc = b; pc < e; pc += 4) cc.words.push_back(ls_word(img.ls.get(), pc));
      chunks.emplace(h, std::move(cc));
    }
  }
  std::error_code ec;
  fs::create_directories(out, ec);

  // Function bodies, split into translation units of bounded size.
  constexpr size_t kInsnsPerUnit = 12000;
  std::vector<std::string> units(1, kPrologue);
  size_t unit_insns = 0, total_insns = 0;
  for (const auto& [h, cc] : chunks) {
    if (unit_insns >= kInsnsPerUnit) {
      units.push_back(kPrologue);
      unit_insns = 0;
    }
    units.back() += emit_chunk(cc);
    unit_insns += cc.words.size();
    total_insns += cc.words.size();
  }
  // Validation table.
  std::string tab = kPrologue;
  for (const auto& [h, cc] : chunks) tab += ac4::strprintf("bool spu_c_%016llx(R::Thread& t);\n", (unsigned long long)h);
  for (const auto& [h, cc] : chunks) {
    tab += ac4::strprintf("static const u32 r_%016llx[] = {", (unsigned long long)h);
    for (auto [b, e] : cc.chunk.ranges) tab += ac4::strprintf("0x%x,0x%x,", b, e);
    tab += "};\n";
    tab += ac4::strprintf("static const u32 k_%016llx[] = {", (unsigned long long)h);
    for (size_t i = 0; i < cc.words.size(); ++i)
      tab += ac4::strprintf("%s0x%08xu,", i % 8 == 0 ? "\n  " : "", __builtin_bswap32(cc.words[i]));  // LS byte order
    tab += "};\n";
  }
  tab += "\nnamespace spu_gen {\nconst Chunk g_chunks[] = {\n";
  for (const auto& [h, cc] : chunks)
    tab += ac4::strprintf("  {0x%05xu, %zuu, r_%016llx, k_%016llx, &spu_c_%016llx},\n", cc.chunk.entry, cc.chunk.ranges.size(),
                           (unsigned long long)h, (unsigned long long)h, (unsigned long long)h);
  if (chunks.empty()) tab += "  {},\n";
  tab += ac4::strprintf("};\nconst u32 g_chunk_count = %zuu;\n}  // namespace spu_gen\n", chunks.size());

  // Write only files whose contents changed; remove stale units.
  std::set<std::string> written;
  auto put = [&](const std::string& name, const std::string& text) {
    fs::path p = out / name;
    written.insert(name);
    std::vector<u8> old;
    if (fs::exists(p, ec)) old = ac4::read_file(p.string());
    if (old.size() == text.size() && std::equal(old.begin(), old.end(), text.begin())) return;
    ac4::write_text(p.string(), text);
  };
  for (size_t i = 0; i < units.size(); ++i) put(ac4::strprintf("spu_gen_%03zu.cpp", i), units[i]);
  put("spu_chunks.cpp", tab);
  for (const auto& e : fs::directory_iterator(out, ec))
    if (e.path().extension() == ".cpp" && !written.count(e.path().filename().string())) fs::remove(e.path(), ec);
  std::printf("%zu record(s), %zu chunk(s), %zu instruction(s), %zu unit(s) -> %s\n", records, chunks.size(), total_insns,
              units.size(), out.string().c_str());
  return 0;
}
