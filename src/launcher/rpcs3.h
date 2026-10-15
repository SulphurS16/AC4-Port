// Preparing the player's decrypted files with the player's own RPCS3, in one step (user decision
// 2026-10-15). The launcher copies RPCS3's program files (never its settings or data) into a work
// folder; that private copy installs the PS3 system software update file the player chose
// (--installfw), decrypts the 10 system modules and a copy of the game's EBOOT.BIN (--decrypt)
// and installs the game's update package when one lies beside the game or the update file
// (--installpkg). The results go to the launcher's own folder of decrypted files; the work folder
// is deleted. The player's RPCS3 is not changed, and the port itself contains no keys and decrypts
// nothing: RPCS3 does.
#pragma once
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace launcher {

namespace fs = std::filesystem;
using Clock_t = long long;  // milliseconds (steady clock)

// The version a PS3UPDAT.PUP declares ("4.93"; its version entry, plain text), or empty.
std::string pup_version(const fs::path& pup);
// The system software installed in an RPCS3 folder ("4.93"), or empty.
std::string rpcs3_system_software(const fs::path& rpcs3_exe);
// An update package of the game (BLUS30187 ... .pkg) in or below these folders, or empty.
fs::path find_update_pkg(const fs::path& disc, const fs::path& also);

struct Rpcs3Job {
  fs::path rpcs3_exe;   // the player's RPCS3
  fs::path pup;         // PS3UPDAT.PUP of system software 4.93 (unused when RPCS3 has 4.93 already)
  fs::path disc;        // the game folder (PS3_GAME\USRDIR\EBOOT.BIN)
  fs::path update_pkg;  // optional
  fs::path work;        // a work folder (deleted afterwards)
  fs::path out;         // the decrypted files go here
};

class Rpcs3Prep {
 public:
  ~Rpcs3Prep();
  void start(const Rpcs3Job& job, std::function<void()> notify);  // notify: from the worker
  void cancel();
  bool running() const { return running_.load(); }
  struct View {
    std::string status;
    bool finished = false, ok = false;
    std::string error, note;  // note: what was left out (the optional update)
  };
  View view();

 private:
  void run(Rpcs3Job job);
  void set_status(const std::string& s);
  void log(const std::string& s);  // <install>\prepare-files.log (no personal data)
  std::mutex mu_;
  View view_;
  std::function<void()> notify_;
  std::thread worker_;
  std::atomic<bool> running_{false}, cancel_{false};
  void* job_object_ = nullptr;
  FILE* log_ = nullptr;
  Clock_t t0_ = 0;
};

}  // namespace launcher
