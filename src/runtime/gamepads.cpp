// Game controllers: XInput and PlayStation controllers through Windows' HID (gamepads.h).
//
// PlayStation controllers send HID input reports, read here without a driver or helper program:
//   DUALSHOCK 4: report 0x01 over USB, and over Bluetooth until a program asks for the full
//     reports; then 0x11-0x19 with the same data 2 bytes later. Data: left X, left Y, right X,
//     right Y (0-255, 128 centre, Y down), hat (low 4 bits: 0 up, clockwise to 7, 8 none) with
//     Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80; then L1 0x01, R1 0x02, L2 0x04,
//     R2 0x08, Share 0x10, Options 0x20, L3 0x40, R3 0x80; then PS 0x01, touchpad 0x02; then the
//     L2 and R2 travel (0-255).
//   DualSense / DualSense Edge: report 0x01 over USB (64 bytes) and 0x31 over Bluetooth (one byte
//     later): sticks as above, L2 and R2 travel, a counter, then the DUALSHOCK 4's three button
//     bytes (Create for Share; mute 0x04 beside PS and touchpad). Over Bluetooth before a program
//     asks for the full reports: report 0x01 in the DUALSHOCK 4's layout.
// One thread per controller waits for its reports; a watcher looks for new controllers every
// 1.5 s. Nothing is written to the controllers.
#include "runtime/gamepads.h"

#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "runtime/log.h"

namespace rt::pads {
namespace {

using Clock = std::chrono::steady_clock;

// ---- XInput, loaded from the OS at run time (xinput1_4 ships with Windows 8+; 9_1_0 as fallback)
using XGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XGetState g_xget = nullptr;
XSetState g_xset = nullptr;

void load_xinput() {
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE h = LoadLibraryW(L"xinput1_4.dll");
    if (!h) h = LoadLibraryW(L"xinput9_1_0.dll");
    if (!h) {
      LOG_W("pad", "XInput not available: Xbox controllers are not read");
      return;
    }
    g_xget = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(h, "XInputGetState")));
    g_xset = reinterpret_cast<XSetState>(reinterpret_cast<void*>(GetProcAddress(h, "XInputSetState")));
  });
}

// ---- PlayStation controllers ----
constexpr u16 kSony = 0x054C;
struct Known {
  u16 pid;
  Model model;
  const char* name;
};
const Known kKnown[] = {
    {0x05C4, Model::DualShock4, "DUALSHOCK 4"},
    {0x09CC, Model::DualShock4, "DUALSHOCK 4"},
    {0x0BA0, Model::DualShock4, "DUALSHOCK 4 (USB wireless adaptor)"},
    {0x0CE6, Model::DualSense, "DualSense"},
    {0x0DF2, Model::DualSense, "DualSense Edge"},
};

struct Device {
  std::wstring path;
  Model model = Model::DualSense;
  bool bluetooth = false;
  const char* name = "";
  HANDLE h = INVALID_HANDLE_VALUE;
  HANDLE stop = nullptr;
  size_t report_len = 64;
  u64 order = 0;  // connection order: the order of the slots after the XInput controllers
  std::thread reader;
  std::atomic<bool> alive{true};
  std::mutex mu;
  XINPUT_GAMEPAD pad{};
  DWORD packet = 0;
  bool have = false;  // a state report arrived
};

std::mutex g_dev_mu;
std::vector<std::unique_ptr<Device>> g_devs;
std::set<std::wstring> g_other;  // HID devices that are not PlayStation controllers (not opened again)
u64 g_order = 0;

const char* link_name(const Device& d) { return d.bluetooth ? "Bluetooth" : "USB"; }

void read_reports(Device* d) {
  std::vector<u8> buf(std::max<size_t>(d->report_len, 64));
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE waits[2] = {ov.hEvent, d->stop};
  for (;;) {
    ResetEvent(ov.hEvent);
    DWORD got = 0;
    if (!ReadFile(d->h, buf.data(), DWORD(buf.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {  // stopping
      CancelIoEx(d->h, &ov);
      GetOverlappedResult(d->h, &ov, &got, TRUE);
      break;
    }
    if (!GetOverlappedResult(d->h, &ov, &got, FALSE)) break;  // unplugged or switched off
    XINPUT_GAMEPAD p{};
    if (!got || !parse_report(d->model, d->bluetooth, buf.data(), got, &p)) continue;
    std::lock_guard lk(d->mu);
    if (!d->have) LOG_I("pad", "%s (%s): input report 0x%02x", d->name, link_name(*d), buf[0]);
    if (!d->have || std::memcmp(&p, &d->pad, sizeof p) != 0) ++d->packet;
    d->pad = p;
    d->have = true;
  }
  CloseHandle(ov.hEvent);
  d->alive = false;
}

// Opens a HID device that is a PlayStation controller this code reads; nullptr otherwise.
std::unique_ptr<Device> open_device(const std::wstring& path, bool* other) {
  *other = false;
  // No access rights: enough for the vendor and product ids, and opens any HID device.
  HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (probe == INVALID_HANDLE_VALUE) return nullptr;
  HIDD_ATTRIBUTES a{};
  a.Size = sizeof a;
  bool got = HidD_GetAttributes(probe, &a);
  CloseHandle(probe);
  const Known* k = nullptr;
  for (const Known& c : kKnown)
    if (got && a.VendorID == kSony && a.ProductID == c.pid) k = &c;
  if (!k) {
    *other = got;
    return nullptr;
  }
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;  // another program holds it alone (asked again later)
  PHIDP_PREPARSED_DATA pp = nullptr;
  HIDP_CAPS caps{};
  bool capsok = HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
  if (pp) HidD_FreePreparsedData(pp);
  if (!capsok || caps.UsagePage != 0x01 || (caps.Usage != 0x04 && caps.Usage != 0x05)) {  // not its gamepad collection
    CloseHandle(h);
    *other = true;
    return nullptr;
  }
  auto d = std::make_unique<Device>();
  d->path = path;
  d->model = k->model;
  d->name = k->name;
  d->h = h;
  d->report_len = caps.InputReportByteLength;
  // Over Bluetooth the device path names the HID service, and the reports are longer than 64 bytes.
  std::wstring low = path;
  for (auto& ch : low) ch = wchar_t(towlower(ch));
  d->bluetooth = low.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos || caps.InputReportByteLength > 64;
  return d;
}

void scan() {
  GUID hid{};
  HidD_GetHidGuid(&hid);
  HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return;
  std::vector<std::wstring> paths;
  for (DWORD i = 0;; ++i) {
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd)) break;
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
    if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<u8> buf(need);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) paths.push_back(det->DevicePath);
  }
  SetupDiDestroyDeviceInfoList(set);

  std::vector<std::unique_ptr<Device>> gone;
  {
    std::lock_guard lk(g_dev_mu);
    for (auto it = g_devs.begin(); it != g_devs.end();) {  // controllers unplugged or switched off
      if ((*it)->alive) {
        ++it;
        continue;
      }
      gone.push_back(std::move(*it));
      it = g_devs.erase(it);
    }
  }
  for (auto& d : gone) {
    if (d->reader.joinable()) d->reader.join();
    CloseHandle(d->h);
    CloseHandle(d->stop);
    LOG_I("pad", "%s (%s) disconnected", d->name, link_name(*d));
  }
  for (const std::wstring& p : paths) {
    bool known = false;
    {
      std::lock_guard lk(g_dev_mu);
      known = g_other.count(p) != 0;
      for (const auto& d : g_devs) known = known || d->path == p;
    }
    if (known) continue;
    bool other = false;
    std::unique_ptr<Device> d = open_device(p, &other);
    if (!d) {
      if (other) {
        std::lock_guard lk(g_dev_mu);
        g_other.insert(p);
      }
      continue;
    }
    d->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    LOG_I("pad", "PlayStation controller connected: %s over %s (input reports of %zu bytes)", d->name, link_name(*d), d->report_len);
    std::lock_guard lk(g_dev_mu);
    d->order = ++g_order;
    Device* raw = d.get();
    g_devs.push_back(std::move(d));
    raw->reader = std::thread(read_reports, raw);
  }
}

void start_watcher() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      for (;;) {
        scan();
        Sleep(1500);
      }
    }).detach();
  });
}

// ---- the slots ----
struct Entry {
  XINPUT_STATE st{};
  int xindex = -1;  // XInput user index, -1 for a PlayStation controller
};
std::mutex g_mu;
std::vector<Entry> g_snap;
Clock::time_point g_snap_at{};
bool g_x_on[4] = {};
Clock::time_point g_x_retry[4] = {};

// The controllers now: one snapshot serves every reader within 2 ms. A missing XInput controller
// is asked for again once a second (asking for one that is not there can take long).
void refresh() {
  auto now = Clock::now();
  if (g_snap_at != Clock::time_point{} && now - g_snap_at < std::chrono::milliseconds(2)) return;
  g_snap_at = now;
  g_snap.clear();
  for (DWORD i = 0; i < 4 && g_xget; ++i) {
    if (!g_x_on[i] && now < g_x_retry[i]) continue;
    Entry e;
    e.xindex = int(i);
    g_x_on[i] = g_xget(i, &e.st) == ERROR_SUCCESS;
    if (g_x_on[i]) g_snap.push_back(e);
    else g_x_retry[i] = now + std::chrono::seconds(1);
  }
  std::lock_guard lk(g_dev_mu);
  for (const auto& d : g_devs) {
    if (!d->alive) continue;
    std::lock_guard dl(d->mu);
    if (!d->have) continue;
    Entry e;
    e.st.dwPacketNumber = d->packet;
    e.st.Gamepad = d->pad;
    g_snap.push_back(e);
  }
}

}  // namespace

bool parse_report(Model model, bool bluetooth, const u8* r, size_t n, XINPUT_GAMEPAD* out) {
  if (!r || n < 10) return false;
  const u8* d = nullptr;
  bool full = false;  // the DualSense's full layout (triggers before the buttons)
  if (model == Model::DualShock4) {
    if (r[0] == 0x01) d = r + 1;
    else if (r[0] >= 0x11 && r[0] <= 0x19 && n >= 12) d = r + 3;
  } else {
    if (r[0] == 0x01 && !bluetooth && n >= 11) d = r + 1, full = true;
    else if (r[0] == 0x01 && bluetooth) d = r + 1;
    else if (r[0] == 0x31 && n >= 12) d = r + 2, full = true;
  }
  if (!d) return false;
  u8 lx = d[0], ly = d[1], rx = d[2], ry = d[3];
  u8 b0, b1, b2, l2, r2;
  if (full) {
    l2 = d[4], r2 = d[5], b0 = d[7], b1 = d[8], b2 = d[9];
  } else {
    b0 = d[4], b1 = d[5], b2 = d[6], l2 = d[7], r2 = d[8];
  }
  WORD w = 0;
  static const WORD kHat[8] = {
      XINPUT_GAMEPAD_DPAD_UP,
      XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
      XINPUT_GAMEPAD_DPAD_RIGHT,
      XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
      XINPUT_GAMEPAD_DPAD_DOWN,
      XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
      XINPUT_GAMEPAD_DPAD_LEFT,
      XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT,
  };
  if ((b0 & 0x0F) < 8) w |= kHat[b0 & 0x0F];
  if (b0 & 0x10) w |= XINPUT_GAMEPAD_X;  // Square
  if (b0 & 0x20) w |= XINPUT_GAMEPAD_A;  // Cross
  if (b0 & 0x40) w |= XINPUT_GAMEPAD_B;  // Circle
  if (b0 & 0x80) w |= XINPUT_GAMEPAD_Y;  // Triangle
  if (b1 & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
  if (b1 & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
  if (b1 & 0x10) w |= XINPUT_GAMEPAD_BACK;  // Share / Create
  if (b1 & 0x20) w |= XINPUT_GAMEPAD_START;  // Options
  if (b1 & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;
  if (b1 & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
  if (b2 & 0x02) w |= XINPUT_GAMEPAD_BACK;  // touchpad click
  out->wButtons = w;
  out->bLeftTrigger = l2;
  out->bRightTrigger = r2;
  // XInput's axes: X right, Y up, -32768 to 32767. v * 256 apart from the centre keeps every one
  // of the controller's 256 positions (the pad emulation's (x + 32768) >> 8 gives them back).
  auto x = [](u8 v) { return SHORT((int(v) - 128) * 256); };
  auto y = [](u8 v) { return SHORT((127 - int(v)) * 256); };
  out->sThumbLX = x(lx);
  out->sThumbLY = y(ly);
  out->sThumbRX = x(rx);
  out->sThumbRY = y(ry);
  return true;
}

bool get(int slot, XINPUT_STATE* out) {
  load_xinput();
  start_watcher();
  std::lock_guard lk(g_mu);
  refresh();
  if (slot < 0 || slot >= int(g_snap.size())) return false;
  *out = g_snap[size_t(slot)].st;
  return true;
}

void vibrate(int slot, WORD left, WORD right) {
  load_xinput();
  std::lock_guard lk(g_mu);
  refresh();
  if (slot < 0 || slot >= int(g_snap.size()) || g_snap[size_t(slot)].xindex < 0 || !g_xset) return;
  XINPUT_VIBRATION v{};
  v.wLeftMotorSpeed = left;
  v.wRightMotorSpeed = right;
  g_xset(DWORD(g_snap[size_t(slot)].xindex), &v);
}

}  // namespace rt::pads
