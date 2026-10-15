// HTTPS downloads for the launcher (WinHTTP): the pinned toolchain archives.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace launcher {

// Downloads url to `to` (through a .part file), reporting (received, total) bytes; total is 0
// when the server does not say. Returns "" on success, else the reason.
std::string download(const std::wstring& url, const std::filesystem::path& to,
                     const std::function<void(unsigned long long, unsigned long long)>& progress, const std::atomic<bool>& cancel);

}  // namespace launcher
