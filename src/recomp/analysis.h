// Function discovery and control-flow analysis for the PPU recompiler.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ppu/ppu_insn.h"
#include "recomp/image.h"

namespace recomp {

struct Function {
  u32 entry = 0;
  u32 begin = 0, end = 0;          // address range the traversal may cover: [begin, end)
  bool secondary = false;          // jump-in entry inside another function's range
  std::vector<u32> insns;          // reachable instruction addresses, sorted
  std::set<u32> labels;            // intra-function branch targets
  std::map<u32, std::vector<u32>> switches;  // bctr address -> table targets (by index)
  std::set<u32> calls;             // direct bl targets
  std::set<u32> external;          // branch/fallthrough targets outside [begin,end)
  u32 invalid = 0;                 // count of undecodable reachable words
};

struct AnalysisStats {
  size_t opd_entries = 0, functions = 0, secondary = 0, insns = 0, switches = 0, switch_targets = 0;
  size_t unresolved_bctr = 0, invalid = 0, passes = 0;
  std::vector<u32> unresolved_bctr_sites, invalid_sites;
};

class Analysis {
 public:
  explicit Analysis(const Image& img) : img_(img) {}
  void run(const std::set<u32>& extra_entries = {});

  const std::map<u32, Function>& functions() const { return funcs_; }
  const AnalysisStats& stats() const { return st_; }
  // TOC value (r2) for OPD-described functions.
  const std::map<u32, u32>& toc_of() const { return toc_; }

 private:
  const Image& img_;
  std::map<u32, Function> funcs_;
  std::map<u32, u32> toc_;
  u32 module_toc_ = 0;  // the TOC shared by every OPD of the image (0 if they differ)
  AnalysisStats st_;

  u32 code_end_for(u32 a) const;
 public:
  bool is_stub(u32 a) const;

 private:
  void traverse(Function& f);
  bool detect_switch(Function& f, u32 bctr, std::vector<u32>& targets);
  bool detect_spilled_table(Function& f, u32 bctr, std::vector<u32>& targets);
  bool detect_toc_jump(Function& f, u32 bctr, std::vector<u32>& targets);
};

}  // namespace recomp
