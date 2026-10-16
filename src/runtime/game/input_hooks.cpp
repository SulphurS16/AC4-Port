// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/bindings.h"
#include "runtime/datawatch.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"

namespace ppu {
namespace {

// Publishes the port 0 pad object ("pad") and its normalized stick floats ("sticks": right X,
// right Y, left X, left Y) as bases for --watch-access.
void publish(u32 pad, u32 port_offset, u32 sticks_offset) {
  if (r32(pad + port_offset) != 0) return;
  static std::atomic<u32> logged{0};
  if (logged.exchange(pad) != pad)
    LOG_I("input", "port 0 pad object 0x%08x, stick floats at 0x%08x (right X, right Y, left X, left Y)", pad,
          pad + sticks_offset);
  rt::datawatch::set_base("pad", pad);
  rt::datawatch::set_base("sticks", pad + sticks_offset);
}

// ---- Mouse camera ----
// The player's operator (0xb52584) maps the pad through the input layer (0xb5006c: dead zone and
// curve; stick 0.569 -> 0.205 turn) and hands the AC's commands to its view object through
// virtual setters: slot 6 (0xb6b148) turn, slot 9 (0xb67970, a thunk to slot 8) look pitch, the
// latter negated first when the game's vertical inversion option is set. A setter stores the value
// only while the view's flags allow it (view +16 -> word & 2 for turn, & 4 for pitch). The turn
// command turns the AC body (its yaw rate follows the command through drag); the camera stands
// behind the body and pitches with the AC's look control (0xa372e8).
//
// The mouse moves the camera, not the AC (user decision 2026-10-16): raw counts become angles
// (kRadPerCount per count times the sensitivity; no smoothing, acceleration or dead zone) that
// turn and pitch the camera at once, however fast the mouse moves. The AC then turns toward where
// the camera looks by the game's own turning: the turn command it gets is the stick's, fully
// pushed while the camera is more than kChaseFull away and proportionally less within, so it
// turns at its own speed and stops where the camera points. The game still places the camera
// (distance, framing, collision): the look control has a yaw block beside its pitch block,
// look +4, the camera's yaw relative to the body (radians, right positive), which the game holds
// at 0 (its limits +36 / +40 are 0); written after the look control's update, it turns the
// camera around the AC (radar cone included). The camera uses the body's heading from before the
// frame's physics, so the camera's angle to the body is taken against the heading at the frame's
// start and the camera shows the mouse's heading exactly while the AC turns under it.
//   Other turning stays the game's: the stick or turn keys turn the AC with the camera at its
// angle; once the AC points where the camera does and the mouse rests, the camera is behind it
// again and follows whatever turns it (auto-sighting, scripts). Where the view takes no turn
// (scripted moments) or during a view reset the camera goes back behind the AC.
//   Pitch: the look control's pitch angle (+48) gets the mouse's angle after its update, within
// its limits +80 (up) / +84 (down); mode +68 is 1 while a view reset recentres it.
constexpr u32 kPlayerTurnSetReturn = 0x00b52708;                          // operator: slot 6 call
constexpr u32 kPlayerPitchSetReturns[] = {0x00b52800, 0x00b52c30};       // slot 9: inverted / normal
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadPerCount = kPi / 2000;  // 0.09 degrees: 1000 counts turn 90
// Sign of the pitch that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;
// The AC's look control (0xa372e8: r3 control, r4 its input, f1 frame time) turns (0xa36c6c: the
// yaw block at +4), then reads the pitch command (getter 0xb67cd4: the input's state +48) and
// integrates it (0xa36684, state at control + 48): angle +0 (radians, up positive), velocity +4,
// accumulator +8, mode +20 (1 while a view reset recentres), max speed +24, drag +28, limits +32
// (upper) and +36 (lower); the velocity is zeroed at a limit. Every input class reads its state
// through slot 2; the player's input object holds the state the player's view writes at +12.
constexpr u32 kLookYaw = 4;
constexpr u32 kPitchState = 48;
constexpr u32 kYawRate = 484;       // an AC body's yaw rate (body = turn object +4; dev trace)
constexpr u32 kBodyForward = 432;   // an AC body's rotation row 2: x +432, z +440; heading atan2(x, z), right turns add
constexpr double kChaseFull = 8.0 * kPi / 180;  // the camera further than this from the AC's heading: full turn command
constexpr double kSettled = 0.2 * kPi / 180;    // nearer, with the mouse at rest: the AC points where the camera does
constexpr double kStickTurn = 0.05;             // a turn command from the stick or the turn keys
// The tutorial's steps count an input that lasts 0.5 s without a gap (its monitor 0xb69658: per
// input a hold time of 0.5 s, a release time of 1/12 s). A mouse movement counts as such an input
// from its start until kTutorialHold after its end (pitch: the operator's "pitch input" flag; turn:
// a command of kTutorialTurn when the AC is not turning already, too small to turn it).
constexpr double kTutorialHold = 0.6;
constexpr double kTutorialTurn = 1e-4;

// The camera's state (the game's main thread: the operator, the look control and the AC update all
// run there, in that order).
struct MouseCam {
  u32 body = 0;            // the player's AC body the headings belong to
  double target = 0;       // where the camera looks: a heading in the body's convention (radians)
  double heading = 0;      // the body's heading at this frame's start
  double offset = 0;       // target - heading: the camera's yaw relative to the AC (look +4 this frame)
  double pitch = 0;        // this frame's mouse pitch (radians, up positive)
  double command = 0;      // the turn command the AC got this frame (dev trace)
  double now = 0;          // this frame's time (seconds)
  double turned_at = -1e9, pitched_at = -1e9;  // the mouse's last movement in each axis (the tutorial's steps)
  int turn_dir = 0;        // the direction of the last mouse turn
  bool fresh = false;      // set by the operator's turn call; the look update writes the frame's camera
  bool turning() const { return now - turned_at < kTutorialHold; }
  bool pitching() const { return now - pitched_at < kTutorialHold; }
};
MouseCam g_cam;
LARGE_INTEGER g_last_frame{};
std::atomic<u64> g_turn_calls{0}, g_pitch_calls{0};
std::atomic<int> g_look_trace{0};    // dev: frames of the player's look control still to log (turn_trace)
std::atomic<int> g_lookset_off{-1};  // dev: a float written at look +offset after every update (look_set)
std::atomic<float> g_lookset_val{0};
std::atomic<u32> g_heading_probe[4];  // dev: (sin, cos) pairs of heading matrices (turn_trace; up to four)
std::atomic<u32> g_player_state{0};  // the player's view state (view +12), written by the operator
std::atomic<u32> g_look{0};          // the player AC's look control
std::atomic<u32> g_player_turn{0};   // the player AC's turn object

float f32_of_bits(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
float rf(u32 ea) { return f32_of_bits(r32(ea)); }
void wf(u32 ea, float v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  w32(ea, bits);
}

double wrap(double a) { return std::remainder(a, 2 * kPi); }  // into [-pi, pi]

bool view_allows(u32 view, u32 mask) {
  u32 flags = view ? r32(view + 16) : 0;
  return flags && rt::vm::committed(flags, 4) && (r32(flags) & mask) != 0;
}

// The heading of an AC body (radians; turning right adds), from its rotation.
bool heading_of(u32 body, double* h) {
  if (!body || !rt::vm::committed(body + kBodyForward, 12)) return false;
  double x = rf(body + kBodyForward), z = rf(body + kBodyForward + 8);
  if (!(x * x + z * z > 0.25)) return false;  // not a rotation (and not NaN)
  *h = std::atan2(x, z);
  return true;
}

// Once per game frame, from the operator's turn call (before the pitch call, the look control and
// the AC's update; f1 the turn command from the stick and the turn keys): the mouse's movement
// since the previous frame turns the camera, and the AC gets the command that turns it toward the
// camera.
void mouse_cam_frame(Ctx& ctx, u32 view) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) / double(freq.QuadPart) : 0;
  g_last_frame = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  u32 turn = g_player_turn.load(std::memory_order_relaxed);
  u32 body = turn && rt::vm::committed(turn + 4, 4) ? r32(turn + 4) : 0;
  double h = 0;
  if (!heading_of(body, &h)) {
    g_cam = MouseCam{};
    return;
  }
  if (body != g_cam.body) {  // a new AC (a mission, AC TEST, the tutorial's next part): the camera behind it
    g_cam = MouseCam{};
    g_cam.body = body;
    g_cam.target = g_cam.heading = h;
  }
  double turned_by = wrap(h - g_cam.heading);  // the AC's turn since the previous frame
  g_cam.heading = h;
  // Menus, a pause, a hitch: what the mouse did meanwhile moves nothing.
  bool gap = dt <= 0 || dt > 0.25;
  const rt::bindings::MouseSettings& m = rt::bindings::mouse();
  double t = gap ? 0 : double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;
  double p = gap ? 0 : double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if ((dx || dy) && traced++ < 40)
    LOG_I("input", "mouse camera: %lld, %lld counts over %.1f ms%s", (long long)dx, (long long)dy, dt * 1000, gap ? " (dropped)" : "");
  u32 look = g_look.load(std::memory_order_relaxed);
  bool recentring = look && rt::vm::committed(look + kPitchState + 20, 4) && r32(look + kPitchState + 20) == 1;
  double stick = ctx.f[1];
  bool stick_turns = std::fabs(stick) > kStickTurn;
  // A heading change faster than an AC turns (about 0.03 rad per 1/60 s step at full speed; twice
  // that allowed) is the game setting it (a start position, a scripted turn), as is any change
  // over a gap.
  double steps = std::min(dt, 0.25) * 60;
  bool set_by_game = gap || std::fabs(turned_by) > std::max(0.07 * steps, 0.05);
  if (!view_allows(view, 2) || recentring) {
    g_cam.target = h;  // no turning here (or a view reset): the camera behind the AC
    t = 0;
  } else if (stick_turns || set_by_game) {
    g_cam.target += turned_by;  // the stick turns the AC, or the game set its heading: the camera keeps its angle to it
  }
  g_cam.target = wrap(g_cam.target + t);
  double o = wrap(g_cam.target - h);
  if (t == 0 && std::fabs(o) < kSettled) {  // caught up and the mouse at rest: the camera follows the AC again
    g_cam.target = h;
    o = 0;
  }
  g_cam.offset = o;
  g_cam.command = stick;
  if (o != 0 && !stick_turns) {  // the AC turns toward the camera as with the stick
    g_cam.command = std::clamp(o / kChaseFull, -1.0, 1.0);
    ctx.f[1] = g_cam.command;
  }
  g_cam.pitch = view_allows(view, 4) && !recentring ? p : 0;
  g_cam.now = double(now.QuadPart) / double(freq.QuadPart);
  if (t != 0) {
    g_cam.turned_at = g_cam.now;
    g_cam.turn_dir = t > 0 ? 1 : -1;
  }
  if (p != 0 && view_allows(view, 4)) g_cam.pitched_at = g_cam.now;
  g_cam.fresh = true;
}

// After the player's look control updated: the frame's camera (its yaw against the AC, the mouse's
// pitch within the limits). Nothing when the operator did not run this frame (cutscenes, menus).
void mouse_cam_apply(u32 look) {
  if (!g_cam.fresh) return;
  g_cam.fresh = false;
  if (rt::vm::committed(look + kLookYaw, 4)) wf(look + kLookYaw, float(g_cam.offset));
  u32 p = look + kPitchState;
  if (g_cam.pitch != 0 && rt::vm::committed(p, 40)) {
    double hi = rf(p + 32), lo = rf(p + 36);
    if (lo < hi) wf(p, float(std::clamp(double(rf(p)) + g_cam.pitch, lo, hi)));
  }
  g_cam.pitch = 0;
}

// The operator's turn call (slot 6) of the player: the player's view state, then the mouse.
void player_turn_call(Ctx& ctx) {
  g_turn_calls.fetch_add(1, std::memory_order_relaxed);
  u32 view = u32(ctx.r[3]);
  if (view && rt::vm::committed(view + 12, 4)) {
    u32 state = r32(view + 12);
    if (g_player_state.exchange(state) != state) {
      LOG_I("input", "mouse camera: player view 0x%08x, state 0x%08x", view, state);
      rt::datawatch::set_base("state", state);  // --watch-access state+OFFSET (turn command +32, pitch +48)
    }
  }
  mouse_cam_frame(ctx, view);
}

}  // namespace

// Pad object updates, once per frame and port: cellPadGetData(port, this + port offset + 8), then
// the sticks normalized to floats in [-1, 1] ((v / 255 - 0.5) * 2, Y inverted). Two classes with
// the same layout at different offsets (0x951f94: port +384, sticks +1172; 0x5ead28: port +736,
// sticks +1524).
void hook_pad_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  publish(pad, 384, 1172);
}

void hook_pad_update_b(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  publish(pad, 736, 1524);
}

// View slot 6 (0xb6b148: r3 view, f1 turn command): from the player's operator, the mouse camera's
// frame, which may replace the command with the one that turns the AC toward the camera.
void hook_view_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) player_turn_call(ctx);
  orig(ctx);
}

// The tutorial's view class (vtable 0x10148768) has its own slot 6 (0xb6acf4): the same store, plus
// a flag its steps count ("turn 3 times") when the command is not zero. The AC turning toward the
// camera counts as the stick does; after a mouse turn the AC may stop sooner than the step needs,
// so the turn input goes on (kTutorialTurn) until kTutorialHold after the mouse rested.
void hook_view_turn_set_b(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) {
    player_turn_call(ctx);
    if (std::fabs(ctx.f[1]) < kTutorialTurn && g_cam.turning()) ctx.f[1] = g_cam.turn_dir * kTutorialTurn;
  }
  orig(ctx);
}

// View slot 9 (0xb67970, a thunk to slot 8: r3 view, f1 pitch command, r5 the operator's "pitch
// input" flag): the stick's command goes on unchanged. The tutorial view's slot 8 (0xb6ae5c) marks
// its "adjust view" input on that flag (r32(view +24) +72; slot 8 of the other views ignores it): a
// mouse look raises it from its start until kTutorialHold after the mouse rested.
void hook_view_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  if (lr == kPlayerPitchSetReturns[0] || lr == kPlayerPitchSetReturns[1]) {
    g_pitch_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_cam.pitching()) ctx.r[5] = 1;
  }
  orig(ctx);
}

// 0x9fc210 (every frame) and 0x9fc364 (while there is turn input): r3 an AC's controller, r4 its
// input. The player's (its input holds the player's view state) gives the player's turn object,
// r32(r32(r32(controller +4) +152) +260); its +4 is the AC body.
void note_turn_input(Ctx& ctx) {
  u32 ctl = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  if (!state || !input || !rt::vm::committed(input + 12, 4) || r32(input + 12) != state || !rt::vm::committed(ctl + 4, 4)) return;
  u32 a = r32(ctl + 4);
  if (!a || !rt::vm::committed(a + 152, 4)) return;
  u32 b = r32(a + 152);
  if (!b || !rt::vm::committed(b + 260, 4)) return;
  u32 obj = r32(b + 260);
  if (g_player_turn.exchange(obj) != obj) LOG_I("input", "mouse camera: player turn object 0x%08x", obj);
}
void hook_ac_turn_input(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}
void hook_ac_turn_input_b(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}

// The AC's look control (0xa372e8: r3 control, r4 input, f1 frame time). For the player's (its
// input holds the player's view state at +12): the mouse camera after the update.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  bool player = state && input && rt::vm::committed(input + 12, 4) && r32(input + 12) == state;
  if (!player) {
    orig(ctx);
    return;
  }
  double dt = ctx.f[1];
  if (g_look.exchange(look) != look) {
    LOG_I("input", "mouse camera: player look control 0x%08x (input 0x%08x), camera yaw at 0x%08x, pitch at 0x%08x", look, input, look + kLookYaw,
          look + kPitchState);
    rt::datawatch::set_base("look", look);  // --watch-access look+OFFSET
  }
  // Diagnostics: the frame time the game hands its simulation (f1), summarized every 5 s.
  static double sum = 0, lo = 1e9, hi = 0;
  static u64 n = 0;
  static LARGE_INTEGER since{};
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (!since.QuadPart) since = now;
  sum += dt, lo = std::min(lo, dt), hi = std::max(hi, dt), ++n;
  if (double(now.QuadPart - since.QuadPart) / double(freq.QuadPart) >= 5.0) {
    LOG_I("input", "game frame time (player look control): %llu frames, mean %.2f ms, min %.2f, max %.2f", (unsigned long long)n,
          sum / double(n) * 1000, lo * 1000, hi * 1000);
    sum = 0, lo = 1e9, hi = 0, n = 0, since = now;
  }
  orig(ctx);
  mouse_cam_apply(look);
}

}  // namespace ppu

