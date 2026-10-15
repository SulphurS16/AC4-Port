#pragma once
// Personal data kept out of logs: the user's profile folder and account name appear in paths;
// logs replace them so a player can share a log as it is.
#include <string>

namespace ac4 {

// `text` with the profile folder written as %USERPROFILE% and the account name, where it stands
// as a path component or a word of its own, as <user> (both slash styles, any letter case).
std::string redact_personal(const std::string& text);

}  // namespace ac4
