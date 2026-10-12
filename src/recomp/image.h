// Read-only view of a PS3 PPU ELF for analysis: loadable segments, sections, OPD and imports.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace recomp {

struct Segment {
  u32 type, flags;
  u32 vaddr, filesz, memsz;
  u64 offset;
};

struct Section {
  u32 type, flags;
  u32 addr, size;
  u64 offset;
  bool exec() const { return flags & 4; }
  bool alloc() const { return flags & 2; }
};

struct Import {
  std::string module;
  u32 fnid;
  u32 slot;  // address of the stub-table slot the loader fills with an OPD address
};

class Image {
 public:
  explicit Image(std::vector<u8> elf);

  bool mapped(u32 va, u32 len = 1) const;
  u32 r32(u32 va) const;  // throws if unmapped
  u8 r8(u32 va) const;
  std::string cstr(u32 va, size_t maxlen = 256) const;

  bool in_code(u32 va) const;  // inside an executable section

  u32 entry_opd() const { return entry_; }
  const std::vector<Segment>& segments() const { return segs_; }
  const std::vector<Section>& sections() const { return secs_; }
  const std::vector<Import>& imports() const { return imports_; }
  const std::vector<std::pair<u32, u32>>& code_ranges() const { return code_; }

  // OPD-like table: the data section whose entries are (code addr, toc) pairs.
  struct Opd {
    u32 addr = 0, size = 0;
  };
  const Opd& opd() const { return opd_; }
  // All function descriptors found: (code address, toc). For executables these come from the OPD
  // section; for relocated PRX modules from exports plus a scan of data for (code, module TOC).
  const std::vector<std::pair<u32, u32>>& opd_entries() const { return opd_entries_; }
  bool is_module() const { return modinfo_ != 0; }
  u32 module_toc() const { return module_toc_; }
  std::string module_name() const { return module_name_; }
  // Exported function descriptors of a module: (library, fnid, opd address).
  struct Export {
    std::string lib;
    u32 fnid, opd;
  };
  const std::vector<Export>& exports() const { return exports_; }

  // TLS / process parameters.
  u32 tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 0;
  u32 proc_param = 0, prx_param = 0;

 private:
  std::vector<u8> d_;
  std::vector<Segment> segs_;
  std::vector<Section> secs_;
  std::vector<std::pair<u32, u32>> code_;  // [begin, end) of executable sections
  std::vector<Import> imports_;
  Opd opd_;
  u32 entry_ = 0;
  u32 modinfo_ = 0, module_toc_ = 0;
  std::string module_name_;
  std::vector<std::pair<u32, u32>> opd_entries_;
  std::vector<Export> exports_;
  void parse_stub_table(u32 begin, u32 end);
  const u8* ptr(u32 va, u32 len) const;
};

}  // namespace recomp
