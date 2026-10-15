// INI files edited in place (the shared settings file of the launcher and the game): comments,
// blank lines and key order survive when values change. Sections and keys are case-insensitive;
// keys before the first [section] belong to section "". Full-line comments start with '#' or ';'.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ac4 {

class Ini {
 public:
  // Reads `path`; a missing file gives an empty document (throws only on read errors).
  static Ini load(const std::filesystem::path& path);
  static Ini parse(const std::string& text);
  std::string text() const;
  // Writes atomically (temporary file, then rename), through guard_write.
  void save(const std::filesystem::path& path) const;

  bool empty() const { return lines_.empty(); }
  bool has_section(const std::string& section) const;
  std::optional<std::string> get(const std::string& section, const std::string& key) const;
  std::string get_or(const std::string& section, const std::string& key, const std::string& fallback) const;
  // Sets an existing key's value in place, or adds the key at the end of its section (the section
  // is created at the end of the file when missing).
  void set(const std::string& section, const std::string& key, const std::string& value);
  bool remove(const std::string& section, const std::string& key);
  // The keys and values of a section in file order.
  std::vector<std::pair<std::string, std::string>> entries(const std::string& section) const;
  // Appends a comment or blank line to a section (created when missing).
  void add_line(const std::string& section, const std::string& line);

 private:
  struct Line {
    std::string raw;      // the line as written (rewritten when its value changes)
    std::string section;  // lower-case section the line belongs to
    std::string key;      // lower-case key (key lines only)
    std::string value;
    bool is_key = false, is_header = false;
  };
  size_t section_end(const std::string& lsection) const;  // index after the section's last line
  std::vector<Line> lines_;
};

}  // namespace ac4
