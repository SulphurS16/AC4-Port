#include "launcher/process.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwctype>

namespace launcher {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring q = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
    } else if (c == L'"') {
      q.append(slashes * 2 + 1, L'\\');
      slashes = 0;
    } else {
      slashes = 0;
    }
    q += c;
  }
  q.append(slashes, L'\\');
  return q + L"\"";
}

Runner::Runner() {
  job_ = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
}

Runner::~Runner() {
  if (job_) CloseHandle(job_);  // ends anything still running
}

void Runner::cancel() {
  cancelled_ = true;
  if (job_) TerminateJobObject(job_, 1);
}

RunResult Runner::run(const std::filesystem::path& exe, const std::vector<std::wstring>& args, const std::filesystem::path& cwd,
                      const std::wstring& path_prefix, const std::function<void(const std::string&)>& on_line) {
  RunResult r;
  if (cancelled_) {
    r.cancelled = true;
    return r;
  }
  std::wstring cmd = quote_arg(exe.wstring());
  for (const auto& a : args) cmd += L" " + quote_arg(a);
  // Environment: this process's, with PATH extended.
  std::wstring env;
  {
    wchar_t* block = GetEnvironmentStringsW();
    bool path_done = false;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      std::wstring var = p;
      if (!path_prefix.empty() && _wcsnicmp(var.c_str(), L"PATH=", 5) == 0) {
        var = L"PATH=" + path_prefix + L";" + var.substr(5);
        path_done = true;
      }
      env += var;
      env += L'\0';
    }
    FreeEnvironmentStringsW(block);
    if (!path_done && !path_prefix.empty()) {
      env += L"PATH=" + path_prefix;
      env += L'\0';
    }
    env += L'\0';
  }
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    r.error = "cannot create a pipe";
    return r;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                           env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  DWORD err = GetLastError();
  CloseHandle(wr);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    CloseHandle(rd);
    r.error = "cannot start " + narrow(exe.filename().wstring()) + " (error " + std::to_string(err) + ")";
    return r;
  }
  if (job_) AssignProcessToJobObject(job_, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  r.started = true;
  std::string pending;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
    pending.append(buf, n);
    size_t start = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
      if (pending[i] != '\n' && pending[i] != '\r') continue;
      if (i > start) on_line(pending.substr(start, i - start));
      start = i + 1;
    }
    pending.erase(0, start);
  }
  if (!pending.empty()) on_line(pending);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  GetExitCodeProcess(pi.hProcess, &r.exit_code);
  CloseHandle(pi.hProcess);
  r.cancelled = cancelled_.load();
  return r;
}

}  // namespace launcher
