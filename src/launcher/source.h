// The port's sources for a release launcher (user decision 2026-10-16: the release zip holds only
// AC4-Launcher.exe and its readme). The exported source tree travels inside the launcher as a zip
// (resource 2, RT_RCDATA; CMake option AC4_SOURCE_ZIP, set by tools/release/export.py) and is
// unpacked into <install folder>\source when that copy is missing or from another release. A
// launcher that sits in a source tree (development, or a copy of the repository) uses that tree.
#pragma once
#include <filesystem>
#include <string>

namespace launcher {

namespace fs = std::filesystem;

// The launcher carries the port's sources.
bool has_embedded_source();
// dir holds this launcher's sources (unpacked earlier, same release).
bool source_current(const fs::path& dir);
// Unpacks the launcher's sources into dir unless they are current there (an older copy is
// replaced as a whole). Returns "" or what went wrong.
std::string unpack_source(const fs::path& dir);

}  // namespace launcher
