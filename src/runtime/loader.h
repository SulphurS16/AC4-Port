#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt {

struct ExecInfo {
  u32 entry_opd = 0;
  u32 sdk_version = 0;
  s32 primary_prio = 1001;
  u32 primary_stack = 0x10000;
  u32 malloc_pagesize = 0x100000;
  u32 tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0;
  u32 image_end = 0;
  u32 segs_size = 0;  // LOAD segments in 64 KiB pages: charged to the process memory budget
  size_t imports = 0, imports_unimplemented = 0, imports_unknown = 0, imports_lle = 0;
};

// Maps the executable into guest memory and binds its imports (LLE module exports first, then
// HLE functions).
ExecInfo load_exec(const std::vector<u8>& elf);

// Maps a firmware module image relocated by ac4-prep (fixed addresses) and registers its
// exports. Returns the module name. Imports are bound by bind_lle_imports() once every module is
// mapped (modules import from each other).
std::string load_lle_module(const std::vector<u8>& elf);
void bind_lle_imports();
bool find_lle_export(const std::string& lib, u32 fnid, u32* opd);

struct LleModule {
  std::string name;                // module info name, e.g. "cellSail_Library"
  u32 imports_begin = 0, imports_end = 0;
  u32 start_opd = 0, stop_opd = 0; // module_start / module_stop (0 if none)
  std::vector<std::pair<u32, u32>> data_segments;  // writable PT_LOAD ranges [va, va + size)
  std::vector<u8> pristine;        // their contents before the first module_start
  u32 refs = 0;                    // sysmodule load count (started while > 0)
};
LleModule* find_lle_module(const std::string& name);

}  // namespace rt
