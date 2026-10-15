// Frame rate policy for AC4: the configured frame rate applies in missions, menus and movies run
// at up to 60 Hz (frame_policy.cpp).
#pragma once
#include <string>

namespace rt::game {

// A guest thread started / exited (ppu_thread.cpp).
void thread_started(const std::string& name);
void thread_exited(const std::string& name);
// Gameplay runs: the AC's fire control thread inside a sortie scene (missions, tutorial, AC TEST,
// arena; frame_policy.cpp).
bool mission_running();

}  // namespace rt::game
