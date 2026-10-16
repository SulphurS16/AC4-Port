#include "launcher/rpcs3.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <set>
#include <vector>

#include "common/known_inputs.h"
#include "common/privacy.h"
#include "launcher/process.h"

namespace launcher {
namespace {

namespace ki = ac4::inputs;
using Clock = std::chrono::steady_clock;

bool is_file(const fs::path& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = wchar_t(towlower(c));
  return s;
}

// RPCS3's data folders: never copied (the private copy starts with empty ones of its own).
bool data_folder(const std::wstring& name) {
  static const std::set<std::wstring> kData = {L"config", L"guiconfigs", L"games", L"log", L"logs", L"cache", L"captures", L"savestates",
                                               L"shaderlog", L"sounds", L"spu_progs", L"ppu_progs", L"patches", L"custom_configs",
                                               L"input_configs", L"screenshots", L"test"};
  std::wstring n = lower(name);
  return n.rfind(L"dev_", 0) == 0 || kData.count(n) != 0;
}

// The private copy's settings: no first-start welcome, no "installed" message boxes, no update
// check, English window titles (the launcher watches them), its game window minimized (after a
// system software install RPCS3 starts building a cache in it until the launcher stops it).
const char kSettings[] =
    "[Meta]\nattachCommandLine=false\n\n[Localization]\nlanguage=en\n\n[main_window]\ninfoBoxEnabledWelcome=false\n"
    "infoBoxEnabledInstallPUP=false\ninfoBoxEnabledInstallPKG=false\ncheckUpdateStart=false\n\n[GSFrame]\nvisibility=Minimized\n";

// The visible top-level windows of a process.
std::vector<std::pair<HWND, std::wstring>> windows_of(DWORD pid) {
  struct Ctx {
    DWORD pid;
    std::vector<std::pair<HWND, std::wstring>> out;
  } ctx{pid, {}};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* c = reinterpret_cast<Ctx*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p == c->pid && IsWindowVisible(h)) {
      wchar_t t[256] = {};
      GetWindowTextW(h, t, 256);
      c->out.push_back({h, t});
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&ctx));
  return ctx.out;
}

HWND window_titled(DWORD pid, const std::wstring& part) {
  for (const auto& [h, t] : windows_of(pid))
    if (t.find(part) != std::wstring::npos) return h;
  return nullptr;
}

// RPCS3's main window ("RPCS3 0.0.42-19698-f3f52fed Alpha | master"), its game window ("FPS: ...")
// and its firmware installer need nothing from the player as such; any other window it shows (a
// question, an error) is for the player to read and answer.
bool main_title(const std::wstring& t) { return t.rfind(L"RPCS3 ", 0) == 0 && t.size() > 6 && iswdigit(t[6]); }
bool console(HWND h) {  // RPCS3's console window (titled with its path) while it decrypts
  wchar_t c[64] = {};
  GetClassNameW(h, c, 64);
  return std::wcsstr(c, L"Console") != nullptr;
}
HWND other_window(DWORD pid) {
  for (const auto& [h, t] : windows_of(pid))
    if (!t.empty() && !main_title(t) && t.find(L"FPS:") == std::wstring::npos && t.find(L"Firmware Installer") == std::wstring::npos &&
        !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) && !console(h) && lower(t).find(L"rpcs3.exe") == std::wstring::npos)
      return h;
  return nullptr;
}
// A window counts once it has stayed open for 0.6 s (one that opens for a moment is not a question).
struct Settle {
  HWND h = nullptr;
  std::chrono::steady_clock::time_point since;
  HWND operator()(HWND w, std::chrono::steady_clock::time_point now) {
    if (w != h) {
      h = w;
      since = now;
    }
    return w && now - since > std::chrono::milliseconds(600) ? w : nullptr;
  }
};
bool shown(HWND h) { return h && IsWindow(h) && IsWindowVisible(h); }
// RPCS3 makes its dev_* folders, empty, when it starts; installing the system software fills dev_flash.
bool has_entries(const fs::path& dir) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  return !ec && it != fs::directory_iterator();
}
std::string title_of(HWND h) {
  wchar_t t[256] = {};
  GetWindowTextW(h, t, 256);
  std::string s = narrow(t);
  return s.size() > 48 ? s.substr(0, 45) + "..." : s;
}
// What RPCS3 has read from files so far: it reads the update file (hundreds of MB) only once the
// install is under way, so while its install question is open the count stands still.
unsigned long long read_bytes(HANDLE process) {
  IO_COUNTERS io{};
  return process && GetProcessIoCounters(process, &io) ? io.ReadTransferCount : 0;
}

const std::vector<std::string>& module_names() {
  static const std::vector<std::string> names(ki::kModules, ki::kModules + ki::kModuleCount);
  return names;
}

bool have_modules(const fs::path& ext) {
  for (const std::string& m : module_names())
    if (!is_file(ext / (m + ".sprx"))) return false;
  return is_file(ext / ki::kFlashAtrac.name);
}

}  // namespace

std::string pup_version(const fs::path& pup) {
  std::ifstream f(pup, std::ios::binary);
  unsigned char h[48] = {};
  if (!f.read(reinterpret_cast<char*>(h), sizeof h) || std::memcmp(h, "SCEUF\0\0\0", 8) != 0) return "";
  auto be64 = [](const unsigned char* p) {
    unsigned long long v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
  };
  unsigned long long files = be64(h + 24);
  for (unsigned long long i = 0; i < files && i < 64; ++i) {
    unsigned char e[32] = {};
    f.seekg(std::streamoff(48 + 32 * i));
    if (!f.read(reinterpret_cast<char*>(e), sizeof e)) return "";
    if (be64(e) != 0x100) continue;  // the version entry: plain text
    unsigned long long off = be64(e + 8), len = std::min<unsigned long long>(be64(e + 16), 16);
    std::string v(size_t(len), '\0');
    f.seekg(std::streamoff(off));
    if (!f.read(v.data(), std::streamsize(len))) return "";
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' || v.back() == '\0')) v.pop_back();
    return v;
  }
  return "";
}

std::string rpcs3_system_software(const fs::path& rpcs3_exe) {
  std::ifstream f(rpcs3_exe.parent_path() / "dev_flash" / "vsh" / "etc" / "version.txt");
  std::string line;
  while (std::getline(f, line)) {  // "release:04.9300:"
    if (line.rfind("release:", 0) != 0) continue;
    std::string v = line.substr(8, line.find(':', 8) - 8);
    size_t dot = v.find('.');
    if (dot == std::string::npos || v.size() < dot + 3) return "";
    std::string major = v.substr(0, dot);
    while (major.size() > 1 && major[0] == '0') major.erase(0, 1);
    return major + "." + v.substr(dot + 1, 2);
  }
  return "";
}

Rpcs3Prep::~Rpcs3Prep() {
  cancel();
  if (worker_.joinable()) worker_.join();
  if (job_object_) CloseHandle(job_object_);
}

void Rpcs3Prep::start(const Rpcs3Job& job, std::function<void()> notify) {
  if (running_) return;
  if (worker_.joinable()) worker_.join();
  if (!job_object_) {
    job_object_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;  // RPCS3 ends with the launcher
    SetInformationJobObject(job_object_, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  {
    std::lock_guard lk(mu_);
    view_ = View{};
  }
  notify_ = std::move(notify);
  cancel_ = false;
  running_ = true;
  worker_ = std::thread([this, job] { run(job); });
}

void Rpcs3Prep::cancel() {
  cancel_ = true;
  if (job_object_) TerminateJobObject(job_object_, 1);
}

Rpcs3Prep::View Rpcs3Prep::view() {
  std::lock_guard lk(mu_);
  return view_;
}

void Rpcs3Prep::set_status(const std::string& s) {
  log(s);
  {
    std::lock_guard lk(mu_);
    view_.status = s;
  }
  if (notify_) notify_();
}

void Rpcs3Prep::set_ask(void* window, const std::string& s) {
  log(window ? "asking the player: " + s : "RPCS3's window for the player was closed");
  {
    std::lock_guard lk(mu_);
    view_.ask = window;
    view_.status = s;
  }
  if (notify_) notify_();
}

void Rpcs3Prep::log(const std::string& s) {
  if (!log_) return;
  long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() - t0_;
  std::fprintf(log_, "[%7.1f] %s\n", double(ms) / 1000, ac4::redact_personal(s).c_str());
  std::fflush(log_);
}

void Rpcs3Prep::run(Rpcs3Job job) {
  HANDLE child = nullptr;
  DWORD pid = 0;
  std::error_code lec;
  fs::create_directories(job.work.parent_path(), lec);
  log_ = _wfopen((job.work.parent_path() / "prepare-files.log").c_str(), L"wb");
  t0_ = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  log("Prepare files: RPCS3 " + narrow(job.rpcs3_exe.wstring()) + ", update file " + narrow(job.pup.wstring()) + ", game " +
      narrow(job.disc.wstring()));
  std::string titles_seen;  // the RPCS3 windows last logged
  auto log_windows = [&] {
    std::string t;
    for (const auto& [h, title] : windows_of(pid)) t += (t.empty() ? "" : " | ") + narrow(title);
    if (t != titles_seen) log("RPCS3 windows: " + (t.empty() ? std::string("none") : t));
    titles_seen = t;
  };
  auto stop = [&] {
    if (!child) return;
    DWORD code = 0;
    if (GetExitCodeProcess(child, &code) && code != STILL_ACTIVE) log("RPCS3 exited by itself (status " + std::to_string(code) + ")");
    TerminateProcess(child, 1);
    WaitForSingleObject(child, 10000);
    CloseHandle(child);
    child = nullptr;
  };
  auto start_rpcs3 = [&](const fs::path& exe, const std::vector<std::wstring>& args) {
    std::wstring cmd = quote_arg(exe.wstring());
    for (const std::wstring& a : args) cmd += L" " + quote_arg(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, exe.parent_path().c_str(), &si, &pi))
      return false;
    AssignProcessToJobObject(job_object_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    child = pi.hProcess;
    pid = pi.dwProcessId;
    titles_seen.clear();
    log("started RPCS3 (pid " + std::to_string(pid) + "):" + narrow(cmd.substr(cmd.find(L' '))));
    return true;
  };
  auto exited = [&] { return child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0; };
  std::error_code ec;
  auto finish = [&](bool ok, const std::string& error, const std::string& note = "") {
    stop();
    log(ok ? "done" + (note.empty() ? std::string() : ": " + note) : "failed: " + error);
    if (!ok) {  // RPCS3's own log of the last step, for the player or a report
      std::ifstream in(job.work / "rpcs3" / "log" / "RPCS3.log", std::ios::binary);
      std::vector<std::string> lines;
      for (std::string l; std::getline(in, l);) {
        lines.push_back(l);
        if (lines.size() > 80) lines.erase(lines.begin());
      }
      for (const std::string& l : lines) log("  RPCS3: " + l);
    }
    if (log_) std::fclose(log_);
    log_ = nullptr;
    fs::remove_all(job.work, ec);
    {
      std::lock_guard lk(mu_);
      view_.ask = nullptr;
      view_.finished = true;
      view_.ok = ok;
      view_.error = error;
      view_.note = note;
    }
    running_ = false;
    if (notify_) notify_();
  };
  const std::string cancelled = "Cancelled: the files were not prepared.";

  // 1. RPCS3's program files, without its settings and data.
  set_status("Copying RPCS3's program files...");
  fs::path copy = job.work / "rpcs3";
  fs::remove_all(job.work, ec);
  fs::create_directories(copy, ec);
  for (const auto& e : fs::directory_iterator(job.rpcs3_exe.parent_path(), ec)) {
    if (cancel_) return finish(false, cancelled);
    std::error_code e2;
    if (e.is_regular_file(e2))
      fs::copy_file(e.path(), copy / e.path().filename(), fs::copy_options::overwrite_existing, e2);
    else if (e.is_directory(e2) && !data_folder(e.path().filename().wstring()))
      fs::copy(e.path(), copy / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
    if (e2) return finish(false, "RPCS3's program files could not be copied: " + e2.message());
  }
  if (ec) return finish(false, "RPCS3's folder could not be read: " + ec.message());
  fs::path exe = copy / job.rpcs3_exe.filename();
  fs::create_directories(copy / "GuiConfigs", ec);
  fs::create_directories(copy / "log", ec);
  std::ofstream(copy / "GuiConfigs" / "CurrentSettings.ini", std::ios::binary) << kSettings;

  // What this RPCS3 understands (older versions lack options; unknown ones make it stop).
  std::set<std::string> options;
  {
    Runner r;
    RunResult rr = r.run(exe, {L"--help"}, copy, L"", [&](const std::string& line) {
      size_t a = line.find("--");
      if (a == std::string::npos) return;
      size_t b = line.find_first_of(" <,", a);
      options.insert(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    });
    if (!rr.started) return finish(false, "RPCS3 could not be started: " + rr.error);
  }
  for (const char* o : {"--installfw", "--decrypt"})
    if (!options.count(o)) return finish(false, "This RPCS3 is too old to prepare the files (it lacks " + std::string(o) + "): update it from rpcs3.net.");
  std::vector<std::wstring> base;
  if (options.count("--allow-any-location")) base.push_back(L"--allow-any-location");  // the copy lives in a work folder

  // 2. The system modules: from the player's RPCS3 when it has 4.93 already, else installed in the copy.
  fs::path ext;
  fs::path player_ext = job.rpcs3_exe.parent_path() / "dev_flash" / "sys" / "external";
  if (rpcs3_system_software(job.rpcs3_exe) == ki::kSystemSoftware && have_modules(player_ext)) {
    ext = player_ext;
  } else {
    if (job.pup.empty()) return finish(false, "Choose the PS3 system software 4.93 update file (PS3UPDAT.PUP).");
    const std::string installing = "Installing PS3 system software 4.93 in a private copy of RPCS3 (about half a minute)...";
    set_status(installing);
    std::vector<std::wstring> args = base;
    args.insert(args.end(), {L"--installfw", job.pup.wstring()});
    if (!start_rpcs3(exe, args)) return finish(false, "RPCS3 could not be started.");
    // RPCS3 first asks "Install firmware: <file>?" in a window titled "RPCS3 Firmware Installer" (No
    // has the focus), then shows its progress under the same title. The question is the player's:
    // an installer window while RPCS3 reads nothing is the question (versions that do not ask
    // start reading the update file at once). After Yes RPCS3 reads the update file, shows its
    // progress and fills dev_flash; after No, or with the question closed, nothing happens: then
    // Prepare files stops. Any other window RPCS3 shows (an error, say) is the player's to read
    // and close the same way. Done when the system software is there and the installer has
    // stayed closed for a second; RPCS3 then starts building a cache of its own, which is not
    // needed. Waiting for the player does not count toward the time limit.
    fs::path copy_ext = copy / "dev_flash" / "sys" / "external";
    const std::string question = "RPCS3 asks \"Install firmware: ...\\" + narrow(job.pup.filename().wstring()) +
                                 "?\": click Yes in RPCS3's window to continue (No stops Prepare files).";
    const std::string again = ": nothing was installed or prepared. Click Prepare files to try again.";
    HWND asked = nullptr, installer_seen = nullptr;
    Settle settle;
    bool working = false, asked_question = false, answered = false, answered_question = false;
    std::string asked_title, answered_title;
    unsigned long long io_mark = read_bytes(child);
    auto now = Clock::now(), last = now, open_at = now, seen_at = now, active_at = now;
    auto busy = Clock::duration::zero();
    for (;;) {
      Sleep(200);
      if (cancel_) return finish(false, cancelled);  // after the wait: Cancel ends RPCS3 too
      log_windows();
      now = Clock::now();
      if (!asked) busy += now - last;
      last = now;
      // Signs of the install under way: the update file read (32 MB more each time), dev_flash filled.
      unsigned long long io = read_bytes(child);
      if (io - io_mark > (32ull << 20) || (!working && has_entries(copy / "dev_flash"))) {
        if (!working) log("RPCS3 is installing (" + std::to_string((io - io_mark) >> 20) + " MB read)");
        working = true;
        active_at = now;
        io_mark = io;
      }
      HWND installer = window_titled(pid, L"Firmware Installer");
      if (installer) open_at = active_at = now;
      if (installer != installer_seen) {
        if (installer_seen && !installer && !working && installer_seen != asked) {  // the question, answered before it was asked about
          log("RPCS3's install question was answered at once");
          answered = answered_question = true;
          answered_title.clear();
        }
        installer_seen = installer;
        seen_at = now;
      }
      if (asked && (!shown(asked) || (asked_question && working))) {
        if (shown(asked)) {
          log("RPCS3's installer window shows its progress: nothing to answer");
        } else {
          log(asked_question ? "the player answered RPCS3's install question" : "the player closed RPCS3's window \"" + asked_title + "\"");
          answered = true;
          answered_question = asked_question;
          answered_title = asked_title;
        }
        asked = nullptr;
        set_ask(nullptr, installing);
        active_at = now;
      }
      if (!asked) {  // a window for the player: any but RPCS3's own; the installer while RPCS3 reads nothing
        HWND want = settle(other_window(pid), now);
        bool is_question = false;
        if (!want && installer && !working && now - seen_at > std::chrono::milliseconds(600)) want = installer, is_question = true;
        if (want) {
          asked = want;
          asked_question = is_question;
          asked_title = title_of(want);
          set_ask(want, is_question ? question : "RPCS3 shows \"" + asked_title + "\": read it in RPCS3's window and answer or close it there.");
        }
      }
      if (asked) active_at = now;
      bool installed = rpcs3_system_software(exe) == ki::kSystemSoftware && have_modules(copy_ext);
      if ((installed && !installer && now - open_at > std::chrono::seconds(1)) || window_titled(pid, L"FPS:")) break;
      if (exited()) {
        if (installed) break;
        return finish(false, "RPCS3 was closed before it installed the system software" + again);
      }
      if ((answered || working) && now - active_at > std::chrono::seconds(6)) {  // RPCS3 does nothing more
        if (answered && answered_question && !working) return finish(false, "RPCS3's question was answered No or closed" + again);
        if (answered && !answered_question)
          return finish(false, "RPCS3 stopped after its message" + (answered_title.empty() ? std::string() : " \"" + answered_title + "\"") + again);
        return finish(false, "RPCS3 stopped installing the system software from " + narrow(job.pup.filename().wstring()) + again);
      }
      if (busy > std::chrono::minutes(10)) return finish(false, "RPCS3 did not finish installing the system software in 10 minutes.");
    }
    if (asked) set_ask(nullptr, installing);
    stop();
    std::string v = rpcs3_system_software(exe);
    log("system software in the copy: " + (v.empty() ? std::string("none") : v));
    ext = copy / "dev_flash" / "sys" / "external";
    if (v != ki::kSystemSoftware || !have_modules(ext))
      return finish(false, "RPCS3 could not install the system software from " + narrow(job.pup.filename().wstring()) + (v.empty() ? "." : " (it has " + v + ")."));
  }

  // 3. Copies of the 10 modules and of EBOOT.BIN, decrypted by RPCS3 beside themselves.
  const std::string decrypting = "Decrypting the system modules and the game's EBOOT.BIN with RPCS3...";
  set_status(decrypting);
  fs::path stage = job.work / "files";
  fs::create_directories(stage, ec);
  std::vector<std::wstring> args = base;
  for (const std::string& m : module_names()) {
    fs::copy_file(ext / (m + ".sprx"), stage / (m + ".sprx"), fs::copy_options::overwrite_existing, ec);
    args.insert(args.end(), {L"--decrypt", (stage / (m + ".sprx")).wstring()});
  }
  fs::copy_file(ext / ki::kFlashAtrac.name, stage / ki::kFlashAtrac.name, fs::copy_options::overwrite_existing, ec);
  fs::copy_file(job.disc / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", stage / "EBOOT.BIN", fs::copy_options::overwrite_existing, ec);
  if (ec) return finish(false, "The files could not be copied for decrypting: " + ec.message());
  args.insert(args.end(), {L"--decrypt", (stage / "EBOOT.BIN").wstring()});
  if (!start_rpcs3(exe, args)) return finish(false, "RPCS3 could not be started.");
  // RPCS3 decrypts without a window and exits; a window it shows meanwhile (a question, an error)
  // is the player's to read and answer, as during the install, and that wait is not timed.
  {
    HWND asked = nullptr;
    Settle settle;
    std::string asked_title;
    auto last = Clock::now();
    auto busy = Clock::duration::zero();
    for (;;) {
      Sleep(200);
      if (cancel_) return finish(false, cancelled);
      if (exited()) break;
      log_windows();
      auto now = Clock::now();
      if (!asked) busy += now - last;
      last = now;
      if (asked && !shown(asked)) {
        log("the player closed RPCS3's window \"" + asked_title + "\"");
        asked = nullptr;
        set_ask(nullptr, decrypting);
      }
      if (!asked) {
        if (HWND w = settle(other_window(pid), now)) {
          asked = w;
          asked_title = title_of(w);
          set_ask(w, "RPCS3 shows \"" + asked_title + "\": read it in RPCS3's window and answer or close it there.");
        }
      }
      if (busy > std::chrono::minutes(3)) return finish(false, "RPCS3 did not finish decrypting in 3 minutes.");
    }
    if (asked) set_ask(nullptr, decrypting);
  }
  stop();
  if (!is_file(stage / "EBOOT.elf")) return finish(false, "RPCS3 did not decrypt the game's EBOOT.BIN.");
  for (const std::string& m : module_names())
    if (!is_file(stage / (m + ".prx"))) return finish(false, "RPCS3 did not decrypt " + m + ".sprx.");

  // 4. The launcher's folder of decrypted files.
  set_status("Collecting the files...");
  fs::create_directories(job.out, ec);
  std::vector<std::string> files = {"EBOOT.elf", ki::kFlashAtrac.name};
  for (const std::string& m : module_names()) files.push_back(m + ".prx");
  for (const std::string& f : files) {
    fs::copy_file(stage / f, job.out / f, fs::copy_options::overwrite_existing, ec);
    if (ec) return finish(false, "The files could not be saved in " + narrow(job.out.wstring()) + ": " + ec.message());
  }
  finish(true, "", "");
}

}  // namespace launcher
