// TRUE keyboard+mouse input, injected at the engine's action layer (M1: menus).
//
// The engine digests all input into per-context galib::CGaUserInput instances
// living in the static system singleton (ptr at [0x826E4E54], object
// 0x829E6640). Each instance holds 32 abstract action bits + 32 analog action
// floats, rebuilt every frame by CGaUserInput::Update (rex_sub_82211E28,
// called by the pump 0x821CA940 with f1 = frame delta):
//
//   +0xE44 level   (held action bits)      +0xE48 previous level
//   +0xE4C pressed-edge                    +0xE50 released-edge
//   +0xE54 pressed-edge + auto-repeat      (+0x1058/+0x105C repeat delay/rate)
//   +0xE58/+0xED8/+0xF58/+0xFD8 analog action arrays (32 floats each)
//
// Menus consume instance[1] (singleton+0x256F0): confirm/cancel from the
// pressed-edge word, up/down from the repeat word. Flight consumes
// instance[2] (singleton+0x26854). Per-context keymaps therefore need no mode
// detection: menu keys are injected into [1], flight keys (M2+) into [2] -
// the game's own indirection is the context switch.
//
// Injection: strong-override Update, run the original, then OR keyboard-
// derived bits into the words (edges and auto-repeat computed here from our
// own prev-state, using the instance's OWN repeat delay/rate floats so the
// feel matches the pad exactly). The pad path is untouched: we only ever OR
// (plus one masking rule: a key we still hold masks the spurious released-
// edge the game computes because our bit was in prev but not in its rebuilt
// pad-only level). With ac6_kbm_enabled=false the override tail-calls the
// original and nothing else runs.
//
// Bindings come from ac6_input.toml (next to the exe / working directory),
// hot-reloaded ~1x/second. Missing file = built-in defaults (the approved M0
// table). Full RE map: docs/re/subsystems/input.md.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
// ac6KbmAttachWindow takes a Window on every platform, so this is needed even
// where the listener below is not compiled.
#include <rex/ui/window.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <array>

#include <rex/ui/ui_event.h>
#include <rex/ui/window_listener.h>
#endif

REXCVAR_DEFINE_BOOL(ac6_kbm_enabled, false, "AC6/Enhancements",
                    "Enable keyboard and mouse controls.");
REXCVAR_DEFINE_BOOL(ac6_kbm_log, false, "AC6/Enhancements",
                    "Log keyboard and mouse input diagnostics.")
    .debug_only();
REXCVAR_DEFINE_STRING(ac6_kbm_config, "ac6_input.toml", "AC6/Enhancements",
                      "Path to the keyboard and mouse bindings file. Edits to the "
                      "file are picked up live.");
REXCVAR_DEFINE_BOOL(ac6_kbm_padless, true, "AC6/Enhancements",
                    "Provide a virtual controller when none is connected, so "
                    "keyboard and mouse work on their own.");
REXCVAR_DEFINE_DOUBLE(ac6_cursor_hide_seconds, 3.0, "AC6/Enhancements",
                      "Hide the free mouse cursor after this many seconds without "
                      "motion (0 = never hide).")
    .range(0.0, 300.0);

// The SDK's own mnk virtual-pad driver (input/mnk) - forced off while our
// KB+M is enabled so two keyboard mappers never fight over the same pad.
REXCVAR_DECLARE(bool, mnk_mode);

namespace {

using rex::ui::ParseVirtualKey;
using rex::ui::VirtualKey;

// All [KBM] output goes through the game's standard log (ac6recomp.log):
// operational notices at info, config problems at warn. Visibility follows
// the log_level cvar like every other subsystem.
void KbmLog(const std::string& msg) {
  REXLOG_INFO("[KBM] {}", msg);
}

void KbmWarn(const std::string& msg) {
  REXLOG_WARN("[KBM] {}", msg);
}

// ---- Engine addresses (verified, docs/re/subsystems/input.md) --------------
constexpr uint32_t kSingletonPtrGlobal = 0x826E4E54;
constexpr uint32_t kInstanceOffsets[4] = {0x2458C, 0x256F0, 0x26854, 0x279B8};
constexpr uint32_t kMenuInstance = 0x256F0;    // instance[1]
constexpr uint32_t kFlightInstance = 0x26854;  // instance[2]

constexpr uint32_t kOffLevel = 0xE44;
constexpr uint32_t kOffPressed = 0xE4C;
constexpr uint32_t kOffReleased = 0xE50;
constexpr uint32_t kOffRepeat = 0xE54;
constexpr uint32_t kOffRepeatDelay = 0x1058;  // float: initial auto-repeat delay
constexpr uint32_t kOffRepeatRate = 0x105C;   // float: auto-repeat interval
constexpr uint32_t kOffAnalogA = 0xE58;       // 32 floats
constexpr uint32_t kOffAnalogC = 0xF58;       // 32 floats

// ---- Menu actions as MIRROR bits --------------------------------------------
// Round-4 pad ground truth: every context instance has its OWN action-bit
// layout, derived from per-instance binding masks (inst+4+8+a*4, one u32 of
// MIRROR-space button bits per action slot). So keyboard keys are expressed
// as mirror-bit presses (exactly what the pad produces after the remap in the
// mirror refresh) and translated per instance through its live mask table.
// Mirror bits observed: dpad_down=1, dpad_left=2 (so up=0, right=3), A=5,
// X=6, B=7, START=10, BACK=11.
struct ActionDef {
  const char* name;
  int mirror_bit;
};
constexpr ActionDef kMenuActions[] = {
    {"up", 0},      {"down", 1},   {"left", 2},  {"right", 3},
    {"confirm", 5}, {"cancel", 7}, {"start", 10}, {"back", 11},
};
constexpr size_t kNumMenuActions = std::size(kMenuActions);

// ---- Flight actions (device-level XINPUT effects, active only in flight) ----
struct FlightActionDef {
  const char* name;
  uint16_t buttons;  // XINPUT wButtons bits to assert
  uint8_t lt, rt;    // trigger values to assert (max-combined)
  int16_t lx, ly;    // left-stick deflection to assert (kb overrides pad axis)
  int16_t rx, ry;    // right-stick (camera) deflection
};
// Button identities from field testing. A/B corrected 2026-08-09: the
// M2 round-2 report had these two the wrong way round, which swapped the
// mouse buttons in game (LMB fired missiles, RMB the gun).
// A=machine gun, B=missile/fire, X=map/radar, Y=switch target,
// RS-click=change view, LS-click=gear, BACK=change weapon, START=pause.
// Names and order follow AC7's PC keyboard screen (user request); AC6-only
// actions (pause, wingman, gear) append at the end. AC7 actions with no AC6
// equivalent (flare, radio, highlight-target) are omitted.
constexpr FlightActionDef kFlightActions[] = {
    {"pitch_down", 0, 0, 0, 0, 32767, 0, 0},  // Descend (kb fallback; mouse steers)
    {"pitch_up", 0, 0, 0, 0, -32767, 0, 0},   // Ascend - stick pull
    {"roll_left", 0, 0, 0, -32767, 0, 0, 0},  // Turn Left
    {"roll_right", 0, 0, 0, 32767, 0, 0, 0},  // Turn Right
    {"yaw_left", 0x0100, 0, 0, 0, 0, 0, 0},   // LB
    {"yaw_right", 0x0200, 0, 0, 0, 0, 0, 0},  // RB
    {"accelerate", 0, 0, 255, 0, 0, 0, 0},    // RT (was: throttle)
    {"decelerate", 0, 255, 0, 0, 0, 0, 0},    // LT (was: brake)
    {"fire_machine_gun", 0x1000, 0, 0, 0, 0, 0, 0},  // A
    {"fire_missile", 0x2000, 0, 0, 0, 0, 0, 0},      // B - missile / sp weapon
    {"change_weapon", 0x0020, 0, 0, 0, 0, 0, 0},     // BACK
    // Mode key, no pad effect: while held the mouse drives the camera (right
    // stick) instead of pitch/roll - AC7's "Camera Control Key".
    {"camera_control", 0, 0, 0, 0, 0, 0, 0},
    {"camera_up", 0, 0, 0, 0, 0, 0, 32767},
    {"camera_down", 0, 0, 0, 0, 0, 0, -32767},
    {"camera_left", 0, 0, 0, 0, 0, -32767, 0},
    {"camera_right", 0, 0, 0, 0, 0, 32767, 0},
    {"autopilot", 0x0300, 0, 0, 0, 0, 0, 0},  // LB+RB held = AC6 autopilot
    {"high_g", 0, 255, 255, 0, 0, 0, 0},      // = AC7 "Accelerate + Decelerate"
    {"change_view", 0x0080, 0, 0, 0, 0, 0, 0},       // RS click
    {"switch_radar_map", 0x4000, 0, 0, 0, 0, 0, 0},  // X - map
    {"switch_targets", 0x8000, 0, 0, 0, 0, 0, 0},    // Y
    {"pause", 0x0010, 0, 0, 0, 0, 0, 0},      // START
    {"wingman_up", 0x0001, 0, 0, 0, 0, 0, 0},
    {"wingman_down", 0x0002, 0, 0, 0, 0, 0, 0},
    {"wingman_left", 0x0004, 0, 0, 0, 0, 0, 0},
    {"wingman_right", 0x0008, 0, 0, 0, 0, 0, 0},
    {"gear", 0x0040, 0, 0, 0, 0, 0, 0},       // LS click - landing gear
};
constexpr size_t kNumFlightActions = std::size(kFlightActions);

size_t CameraControlAction() {
  static const size_t idx = [] {
    for (size_t i = 0; i < kNumFlightActions; ++i) {
      if (std::strcmp(kFlightActions[i].name, "camera_control") == 0) return i;
    }
    return size_t{0};
  }();
  return idx;
}

struct MouseConfig {
  // "steer" = mouse drives the left stick (pitch/roll) while flying;
  // "off" disables. ("camera" reserved: right-stick drive, later.)
  std::string mode = "steer";
  // "velocity" (ACAH/AC7 style): deflection tracks current mouse SPEED and
  // snaps back the moment the mouse stops - self-damping.
  // "position": mouse displacement accumulates into a held deflection
  // (the M3 round-1/2 model; uses `recenter`).
  std::string steer_model = "velocity";
  double sensitivity_x = 1.5;  // velocity: full stick at (1000/sens) px/s
  double sensitivity_y = 1.5;  // position: 1.0 ~= 500px for full deflection
  bool invert_x = false;
  bool invert_y = true;   // true = mouse DOWN pitches DOWN (M3 field feedback);
                          // false = pull-down-to-pitch-up, flight-sim style
  double smoothing = 0.035;     // velocity model: EMA time constant, seconds
                                // (jitter filter; 0 = raw, higher = floatier)
  double curve_exponent = 1.0;  // 1.0 linear; >1 softer center
  double deadzone = 0.0;        // 0..1 on the virtual stick
  double anti_deadzone = 0.15;  // jump output past the ENGINE's own stick
                                // deadzone so small motions bite immediately
  double recenter = 3.0;        // position model only: return-to-center rate
                                // (full deflections/second); 0 = stick stays
};

struct Config {
  std::vector<VirtualKey> menu_keys[kNumMenuActions];
  std::vector<VirtualKey> flight_keys[kNumFlightActions];
  MouseConfig mouse;
  // Which CGaUserInput instances receive the [menu] mirror-bit presses.
  // Translation through each instance's own masks makes this bit-exact with
  // real pad presses, so all menu/system contexts are safe targets. [2] is
  // the flight context - refused until M2 gives it its own key set.
  std::vector<int> menu_instances = {0, 1, 3, 4};
};

Config g_config;
std::filesystem::file_time_type g_config_mtime{};
bool g_config_file_seen = false;
uint32_t g_hook_calls = 0;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Scroll wheel: not pollable like keys, so a low-level mouse hook (own
// message-pump thread) stamps short pulses that the binding layer reads as
// two pseudo-keys. VK codes 0x0E/0x0F are unassigned in Win32 - safe IDs.
constexpr VirtualKey kVkWheelUp = static_cast<VirtualKey>(0x0E);
constexpr VirtualKey kVkWheelDown = static_cast<VirtualKey>(0x0F);
std::atomic<int64_t> g_wheel_up_until{0};
std::atomic<int64_t> g_wheel_down_until{0};

#if defined(_WIN32)
LRESULT CALLBACK MouseLLProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
    auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    const int16_t delta = static_cast<int16_t>(HIWORD(info->mouseData));
    const int64_t until = NowMs() + 90;  // ~5 frames: enough for an edge
    if (delta > 0) {
      g_wheel_up_until.store(until, std::memory_order_relaxed);
    } else if (delta < 0) {
      g_wheel_down_until.store(until, std::memory_order_relaxed);
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void EnsureWheelThread() {
  static bool s_started = false;
  if (s_started) return;
  s_started = true;
  std::thread([] {
    if (!SetWindowsHookExW(WH_MOUSE_LL, MouseLLProc, GetModuleHandleW(nullptr), 0)) {
      return;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }).detach();
}
#else
// There is no GetAsyncKeyState equivalent to poll on this platform, so key
// state is accumulated from the SDK window's input events instead. The same
// listener supplies the wheel pulses the Win32 low-level hook produces above,
// so EnsureWheelThread has nothing left to do here.
//
// Only keys, mouse buttons and the wheel are covered. Mouse STEERING is still
// Windows-only: it pins the cursor to the window centre every poll, and
// GTKWindow implements neither cursor warping nor mouse capture (see
// MouseSteerPoll below).
class KbmWindowInput final : public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override { SetKey(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { SetKey(e.virtual_key(), false); }
  void OnMouseDown(rex::ui::MouseEvent& e) override { SetButton(e.button(), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { SetButton(e.button(), false); }

  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    // Same shape as the Win32 hook: a short pulse the binding layer reads as
    // a pseudo-key, so a detent survives until the next poll sees its edge.
    const int32_t dy = e.scroll_y();
    if (!dy) {
      return;
    }
    const int64_t until = NowMs() + 90;
    (dy > 0 ? g_wheel_up_until : g_wheel_down_until).store(until, std::memory_order_relaxed);
  }

  // WindowListener. Focus drives the input gate, exactly as the foreground
  // window check does on Windows; a key still held when focus is lost would
  // otherwise stay stuck down forever, since no key-up ever arrives.
  void OnGotFocus(rex::ui::UISetupEvent&) override {
    focused_.store(true, std::memory_order_relaxed);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    focused_.store(false, std::memory_order_relaxed);
    for (auto& key : down_) {
      key.store(false, std::memory_order_relaxed);
    }
  }

  bool focused() const { return focused_.load(std::memory_order_relaxed); }

  bool IsDown(VirtualKey vk) const {
    const uint16_t code = Canonicalize(vk);
    return code < down_.size() && down_[code].load(std::memory_order_relaxed);
  }

 private:
  // GTK reports only the side-agnostic modifiers (Control_L and Control_R both
  // arrive as kControl), but a binding may name a side - "Left Ctrl" is the
  // stock fire-machine-gun key. Fold the side-specific codes onto the generic
  // one so those bindings resolve rather than silently never matching.
  static uint16_t Canonicalize(VirtualKey vk) {
    switch (static_cast<uint16_t>(vk)) {
      case 0xA0:  // VK_LSHIFT
      case 0xA1:  // VK_RSHIFT
        return static_cast<uint16_t>(VirtualKey::kShift);
      case 0xA2:  // VK_LCONTROL
      case 0xA3:  // VK_RCONTROL
        return static_cast<uint16_t>(VirtualKey::kControl);
      case 0xA4:  // VK_LMENU
      case 0xA5:  // VK_RMENU
        return static_cast<uint16_t>(VirtualKey::kMenu);
      default:
        return static_cast<uint16_t>(vk);
    }
  }

  void SetKey(VirtualKey vk, bool down) {
    const uint16_t code = Canonicalize(vk);
    if (code < down_.size()) {
      down_[code].store(down, std::memory_order_relaxed);
    }
  }

  void SetButton(rex::ui::MouseEvent::Button button, bool down) {
    switch (button) {
      case rex::ui::MouseEvent::Button::kLeft:
        SetKey(VirtualKey::kLButton, down);
        break;
      case rex::ui::MouseEvent::Button::kRight:
        SetKey(VirtualKey::kRButton, down);
        break;
      case rex::ui::MouseEvent::Button::kMiddle:
        SetKey(VirtualKey::kMButton, down);
        break;
      case rex::ui::MouseEvent::Button::kX1:
        SetKey(VirtualKey::kXButton1, down);
        break;
      case rex::ui::MouseEvent::Button::kX2:
        SetKey(VirtualKey::kXButton2, down);
        break;
      default:
        break;
    }
  }

  std::array<std::atomic<bool>, 256> down_ = {};
  std::atomic<bool> focused_{false};
};

KbmWindowInput g_window_input;
std::atomic<bool> g_window_attached{false};

void EnsureWheelThread() {}
#endif

// Accept a few aliases on top of the SDK's canonical key names. The SDK only
// knows generic Control/Alt; the side-specific VKs work directly with
// GetAsyncKeyState, so expose them as pseudo-keys.
VirtualKey ParseKeyName(std::string name) {
  if (name == "Enter") name = "Return";
  else if (name == "Esc") name = "Escape";
  else if (name == "Mouse1") name = "LMB";
  else if (name == "Mouse2") name = "RMB";
  else if (name == "Mouse3") name = "MMB";
  else if (name == "MouseWheelUp") return kVkWheelUp;
  else if (name == "MouseWheelDown") return kVkWheelDown;
  else if (name == "Mouse4" || name == "MouseBack")
    return static_cast<VirtualKey>(0x05);  // VK_XBUTTON1 (side button, "back")
  else if (name == "Mouse5" || name == "MouseForward")
    return static_cast<VirtualKey>(0x06);  // VK_XBUTTON2 (side button, "forward")
  else if (name == "LeftControl" || name == "LeftCtrl")
    return static_cast<VirtualKey>(0xA2);  // VK_LCONTROL
  else if (name == "RightControl" || name == "RightCtrl")
    return static_cast<VirtualKey>(0xA3);  // VK_RCONTROL
  else if (name == "LeftAlt") return static_cast<VirtualKey>(0xA4);   // VK_LMENU
  else if (name == "RightAlt") return static_cast<VirtualKey>(0xA5);  // VK_RMENU
  return ParseVirtualKey(name);
}

void SetDefaultBindings(Config& c) {
  auto set = [&](const char* action, std::initializer_list<const char*> keys) {
    for (size_t i = 0; i < kNumMenuActions; ++i) {
      if (std::strcmp(kMenuActions[i].name, action) == 0) {
        c.menu_keys[i].clear();
        for (const char* k : keys) {
          VirtualKey vk = ParseKeyName(k);
          if (vk != VirtualKey::kNone) c.menu_keys[i].push_back(vk);
        }
      }
    }
  };
  auto setf = [&](const char* action, std::initializer_list<const char*> keys) {
    for (size_t i = 0; i < kNumFlightActions; ++i) {
      if (std::strcmp(kFlightActions[i].name, action) == 0) {
        c.flight_keys[i].clear();
        for (const char* k : keys) {
          VirtualKey vk = ParseKeyName(k);
          if (vk != VirtualKey::kNone) c.flight_keys[i].push_back(vk);
        }
      }
    }
  };
  // AC7-style defaults (user request): 1/2/3 = descend / high-G / ascend as on
  // AC7's keyboard screen, WASD roll/turn + QE yaw, wingman (D-pad) on the
  // arrow keys, camera on the numpad.
  setf("pitch_down", {"1"});
  setf("pitch_up", {"3"});
  setf("roll_left", {"A"});
  setf("roll_right", {"D"});
  setf("yaw_left", {"Q"});
  setf("yaw_right", {"E"});
  setf("accelerate", {"W"});
  setf("decelerate", {"S"});
  setf("fire_machine_gun", {"Mouse1", "LeftControl"});
  setf("fire_missile", {"Mouse2", "Space"});
  setf("change_weapon", {"MouseWheelUp", "MouseWheelDown", "C"});
  setf("camera_control", {"LeftAlt"});
  setf("camera_up", {"Numpad8"});
  setf("camera_down", {"Numpad2"});
  setf("camera_left", {"Numpad4"});
  setf("camera_right", {"Numpad6"});
  setf("autopilot", {"Z", "X"});
  setf("high_g", {"2"});  // AC7's "Accelerate + Decelerate" key (W+S also works)
  setf("change_view", {"V"});
  setf("switch_radar_map", {"R"});
  setf("switch_targets", {"Tab"});
  setf("pause", {"Escape"});
  setf("wingman_up", {"Up"});
  setf("wingman_down", {"Down"});
  setf("wingman_left", {"Left"});
  setf("wingman_right", {"Right"});
  setf("gear", {"G"});

  set("up", {"W", "Up"});
  set("down", {"S", "Down"});
  set("left", {"A", "Left"});
  set("right", {"D", "Right"});
  set("confirm", {"Space", "Mouse1"});  // Space=A (user swap); LMB = confirm
  set("cancel", {"Escape", "Backspace", "Mouse2"});  // RMB = back out
  set("start", {"Enter"});
  set("back", {"Tab"});
}

// Written next to the exe on first run when no config exists. KEEP IN SYNC
// with SetDefaultBindings/MouseConfig - the values here must equal the
// built-in defaults (the file is parsed right after being written, so a
// mismatch or typo shows up as WARN lines in the log).
constexpr const char kDefaultConfigToml[] =
    R"TOML([mouse]
# mode "steer": the mouse flies the plane; "off": no mouse steering.
mode = "steer"
# steer_model "velocity": the stick follows the current mouse SPEED and
# self-centers the moment the mouse stops (AC7/ACAH feel).
# steer_model "position": mouse displacement sets and HOLDS the stick;
# recenter (full deflections/second, 0 = never) eases it back to center.
steer_model = "velocity"
sensitivity_x = 1.5
sensitivity_y = 1.5
invert_x = false
invert_y = true
smoothing = 0.035
curve_exponent = 1.0
deadzone = 0.0
anti_deadzone = 0.15
recenter = 3.0

[menu]
instances = [0, 1, 3, 4]
up      = ["W", "Up"]
down    = ["S", "Down"]
left    = ["A", "Left"]
right   = ["D", "Right"]
confirm = ["Space", "Mouse1"]
cancel  = ["Escape", "Backspace", "Mouse2"]
start   = ["Enter"]
back    = ["Tab"]

[flight]
pitch_down    = ["1"]
pitch_up      = ["3"]
roll_left     = ["A"]
roll_right    = ["D"]
yaw_left      = ["Q"]
yaw_right     = ["E"]
accelerate    = ["W"]
decelerate    = ["S"]
fire_machine_gun = ["Mouse1", "LeftControl"]
fire_missile     = ["Mouse2", "Space"]
change_weapon    = ["MouseWheelUp", "MouseWheelDown", "C"]
camera_control   = ["LeftAlt"]
camera_up     = ["Numpad8"]
camera_down   = ["Numpad2"]
camera_left   = ["Numpad4"]
camera_right  = ["Numpad6"]
autopilot     = ["Z", "X"]
high_g        = ["2"]
change_view   = ["V"]
switch_radar_map = ["R"]
switch_targets   = ["Tab"]
pause         = ["Escape"]
wingman_up    = ["Up"]
wingman_down  = ["Down"]
wingman_left  = ["Left"]
wingman_right = ["Right"]
gear          = ["G"]
)TOML";

void LoadConfig() {
  Config c;
  SetDefaultBindings(c);
  const std::string path = REXCVAR_GET(ac6_kbm_config);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // First run: materialize the default config so users have a commented
    // file to edit (the game runs identically without one).
    bool created = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      created = std::fwrite(kDefaultConfigToml, 1, sizeof(kDefaultConfigToml) - 1, f) ==
                sizeof(kDefaultConfigToml) - 1;
      std::fclose(f);
    }
    if (created) {
      KbmLog(fmt::format("config '{}' not found - created it with the default bindings", path));
      // Fall through and parse the file we just wrote.
    } else {
      KbmWarn(fmt::format("config '{}' not found and could not be created - using built-in "
                          "default bindings", path));
      g_config = c;
      g_config_file_seen = false;
      return;
    }
  }
  try {
    toml::table tbl = toml::parse_file(path);
    if (auto menu = tbl["menu"].as_table()) {
      if (auto insts = (*menu)["instances"].as_array()) {
        c.menu_instances.clear();
        for (auto& el : *insts) {
          if (auto v = el.value<int64_t>()) {
            const int idx = static_cast<int>(*v);
            if (idx == 2) {
              KbmWarn("menu.instances: 2 is the flight context, refused");
            } else if (idx >= 0 && idx <= 4) {
              c.menu_instances.push_back(idx);
            }
          }
        }
      }
      for (size_t i = 0; i < kNumMenuActions; ++i) {
        auto node = (*menu)[kMenuActions[i].name];
        if (auto arr = node.as_array()) {
          c.menu_keys[i].clear();
          for (auto& el : *arr) {
            if (auto s = el.value<std::string>()) {
              VirtualKey vk = ParseKeyName(*s);
              if (vk != VirtualKey::kNone) {
                c.menu_keys[i].push_back(vk);
              } else {
                KbmWarn(fmt::format("menu.{}: unknown key name '{}'", kMenuActions[i].name,
                                    *s));
              }
            }
          }
        }
      }
    }
    if (auto flight = tbl["flight"].as_table()) {
      for (size_t i = 0; i < kNumFlightActions; ++i) {
        auto node = (*flight)[kFlightActions[i].name];
        if (auto arr = node.as_array()) {
          c.flight_keys[i].clear();
          for (auto& el : *arr) {
            if (auto s = el.value<std::string>()) {
              VirtualKey vk = ParseKeyName(*s);
              if (vk != VirtualKey::kNone) {
                c.flight_keys[i].push_back(vk);
              } else {
                KbmWarn(fmt::format("flight.{}: unknown key name '{}'",
                                    kFlightActions[i].name, *s));
              }
            }
          }
        }
      }
    }
    if (auto mouse = tbl["mouse"].as_table()) {
      c.mouse.mode = (*mouse)["mode"].value_or(std::string("steer"));
      c.mouse.steer_model = (*mouse)["steer_model"].value_or(std::string("velocity"));
      c.mouse.sensitivity_x = (*mouse)["sensitivity_x"].value_or(1.5);
      c.mouse.sensitivity_y = (*mouse)["sensitivity_y"].value_or(1.5);
      c.mouse.invert_x = (*mouse)["invert_x"].value_or(false);
      c.mouse.invert_y = (*mouse)["invert_y"].value_or(true);
      c.mouse.smoothing = (*mouse)["smoothing"].value_or(0.035);
      c.mouse.curve_exponent = (*mouse)["curve_exponent"].value_or(1.0);
      c.mouse.deadzone = (*mouse)["deadzone"].value_or(0.0);
      c.mouse.anti_deadzone = (*mouse)["anti_deadzone"].value_or(0.15);
      c.mouse.recenter = (*mouse)["recenter"].value_or(3.0);
    }
    g_config = c;
    g_config_mtime = std::filesystem::last_write_time(path, ec);
    g_config_file_seen = true;
    std::string binds;
    for (size_t i = 0; i < kNumMenuActions; ++i) {
      binds += kMenuActions[i].name;
      binds += "=[";
      for (VirtualKey vk : g_config.menu_keys[i]) {
        binds += rex::ui::VirtualKeyToString(vk);
        binds += ' ';
      }
      binds += "] ";
    }
    KbmLog(fmt::format("config '{}' loaded: instances={} {}", path,
                       fmt::join(g_config.menu_instances, ","), binds));
  } catch (const toml::parse_error& e) {
    KbmWarn(fmt::format("config '{}' parse error: {} - keeping previous bindings", path,
                        std::string(e.description())));
  }
}

void MaybeReloadConfig() {
  // Called from the hook; rate-limited by the caller (~1x/second).
  const std::string path = REXCVAR_GET(ac6_kbm_config);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;  // keep whatever we have (defaults or last good load)
  }
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) return;
  if (!g_config_file_seen || mtime != g_config_mtime) {
    LoadConfig();
  }
}

// The SDK ships its own mnk virtual-pad driver behind the mnk_mode cvar. Two
// keyboard mappers injecting into the same pad would fight, so while our
// KB+M is enabled that cvar is forced off (re-checked on the reload tick in
// case the settings overlay or a toml reload turns it back on).
void EnforceMnkOff() {
  if (REXCVAR_GET(mnk_mode)) {
    REXCVAR_SET(mnk_mode, false);
    KbmLog("mnk_mode was on - forced off (ac6_kbm handles keyboard+mouse)");
  }
}

// Mouse-steering state (used by MouseSteerPoll below). Declared here because
// the input gate consults `capturing`: while steering pins the cursor it OWNS
// the pointer, and overlay mouse ownership is suspended.
struct MouseSteer {
  double x = 0.0, y = 0.0;          // position model: virtual stick, -1..1
  double rate_x = 0.0, rate_y = 0.0;  // velocity model: filtered px/s
  double cam_x = 0.0, cam_y = 0.0;  // camera-control mode: held RS deflection
  bool capturing = false;
  int64_t last_ms = 0;
};
MouseSteer g_mouse;

// ---- Host input gates -------------------------------------------------------
// Input ownership follows desktop window-manager rules: the MOUSE belongs to
// whatever is under the CURSOR, the KEYBOARD to whatever holds FOCUS. The
// ---- Stale-key trust latch --------------------------------------------------
// GetAsyncKeyState reports OS-level async state, which the game cannot clear
// and which can wedge "down" without any real press: the classic case is
// Alt-Tab residue, where Tab's key-UP is swallowed during the focus /
// fullscreen transition and the OS reports Tab held until its next physical
// press. Field-confirmed: a session-long phantom Tab forwarded as held
// BACK/Y, engaging the target-focus camera - "camera dead on keyboard AND
// pad". So a key only counts as held after a fresh up->down transition has
// been observed by us: anything already down at module start, or down at the
// moment the window regains foreground, is ignored (once per offender, with
// a log line naming it) until released. The deliberate flip side: a key
// genuinely held ACROSS an Alt-Tab is eaten until re-pressed - standard
// game-input behavior. Applies to the GetAsyncKeyState path only; the wheel
// pseudo-keys are synthetic and never wedge.
#if defined(_WIN32)
// bit0 = a fresh "up" was observed, the key is trusted; bit1 = wedge logged.
constexpr uint8_t kKeyTrustUpSeen = 1;
constexpr uint8_t kKeyTrustLogged = 2;
std::atomic<uint8_t> g_key_trust[256] = {};
std::atomic<bool> g_focus_was_ok{false};
std::atomic<bool> g_focus_seen{false};  // picks the wedge log wording only

// Called from QueryGate on every gate evaluation; on the unfocused->focused
// edge, drop trust for every key physically down at that instant (their
// up->down did not happen inside the focused session).
void NoteFocusForKeyTrust(bool fg_ok) {
  if (!g_focus_was_ok.exchange(fg_ok, std::memory_order_relaxed) && fg_ok) {
    g_focus_seen.store(true, std::memory_order_relaxed);
    for (int k = 1; k < 256; ++k) {
      if (GetAsyncKeyState(k) & 0x8000) {
        g_key_trust[k].store(0, std::memory_order_relaxed);
      }
    }
  }
}
#endif

// ImGuiDrawer publishes once per frame whether any visible overlay
// window owns the pointer / the keyboard / an active text field - its ImGui
// capture flags conjoined with overlay visibility, so a CLOSED overlay can
// never own input by construction (the focus latch that got a raw
// WantCaptureKeyboard gate rejected here previously cannot form).
// One rule on top, also window-manager semantics (pointer capture): while
// mouse steering holds the pointer - pinned to the window center and hidden -
// there is no cursor to hover an overlay with, so the mouse stays with the
// game until steering releases it (menus, pause, mode "off", focus loss).
// Pad input is deliberately NOT gated: the overlays are not pad-navigable
// (the drawer feeds ImGui keyboard/mouse/touch only), so no overlay can own
// pad input and gating it would change behaviour with no owner on the other
// side.
struct GateState {
  bool fg_ok = false;
  bool capture_mouse = false;     // a visible overlay owns the pointer
  bool capture_keyboard = false;  // a visible overlay owns the keyboard
  bool want_text = false;         // an overlay text field is active
  bool steer_owns_pointer = false;  // mouse steering is pinning the cursor
  bool keys_ok() const { return fg_ok && !capture_keyboard && !want_text; }
  bool mouse_ok() const { return fg_ok && (steer_owns_pointer || !capture_mouse); }
};

GateState QueryGate() {
  GateState g;
#if defined(_WIN32)
  HWND fg = GetForegroundWindow();
  if (fg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    g.fg_ok = (pid == GetCurrentProcessId());
  }
  NoteFocusForKeyTrust(g.fg_ok);
#else
  // Window focus, the equivalent of the foreground-window test above. Until
  // the window is attached nothing can have been pressed anyway, so an
  // unattached gate is closed rather than open.
  g.fg_ok = g_window_attached.load(std::memory_order_relaxed) && g_window_input.focused();
#endif
  g.capture_mouse = rex::ui::ImGuiDrawer::DialogsCaptureMouse();
  g.capture_keyboard = rex::ui::ImGuiDrawer::DialogsCaptureKeyboard();
  g.want_text = rex::ui::ImGuiDrawer::DialogsWantTextInput();
  g.steer_owns_pointer = g_mouse.capturing;
  return g;
}

// LMB/RMB/MMB/X1/X2 and the synthetic wheel keys are MOUSE input (ownership
// follows the cursor); every other key is KEYBOARD input (ownership follows
// focus). The two kinds gate independently, like windows on a desktop.
bool IsMouseKey(VirtualKey vk) {
  switch (vk) {
    case VirtualKey::kLButton:
    case VirtualKey::kRButton:
    case VirtualKey::kMButton:
    case VirtualKey::kXButton1:
    case VirtualKey::kXButton2:
      return true;
    default:
      return vk == kVkWheelUp || vk == kVkWheelDown;
  }
}

bool KeyAllowed(const GateState& gate, VirtualKey vk) {
  return IsMouseKey(vk) ? gate.mouse_ok() : gate.keys_ok();
}

bool KeyHeld(VirtualKey vk) {
  if (vk == kVkWheelUp) {
    return NowMs() < g_wheel_up_until.load(std::memory_order_relaxed);
  }
  if (vk == kVkWheelDown) {
    return NowMs() < g_wheel_down_until.load(std::memory_order_relaxed);
  }
#if defined(_WIN32)
  const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
  std::atomic<uint8_t>& trust = g_key_trust[static_cast<int>(vk) & 0xFF];
  if (!down) {
    // Fresh up: trusted from here on, and the wedge log is rearmed so a
    // NEW wedge episode for the same key gets named again.
    trust.store(kKeyTrustUpSeen, std::memory_order_relaxed);
    return false;
  }
  const uint8_t t = trust.load(std::memory_order_relaxed);
  if (t & kKeyTrustUpSeen) {
    return true;
  }
  if (!(t & kKeyTrustLogged)) {
    trust.store(t | kKeyTrustLogged, std::memory_order_relaxed);
    KbmLog(fmt::format("key 0x{:02X} held {} - ignored until released",
                       static_cast<int>(vk),
                       g_focus_seen.load(std::memory_order_relaxed) ? "across focus gain"
                                                                    : "at startup"));
  }
  return false;
#else
  // Event-driven state (see KbmWindowInput). The Win32 trust dance above is
  // not needed here: a key only enters the table through a real key-down
  // delivered to our window, so a key held from before the window existed, or
  // across a focus change, cannot be seen as pressed - which is what that
  // logic exists to prevent.
  return g_window_input.IsDown(vk);
#endif
}

// ---- Flight-context detection ------------------------------------------------
// The player flight-control sampler (0x82191AE8, sole caller = the player
// update 0x82191480) runs only while the flight sim is actually stepping.
// Its pass-through hook below timestamps each run; "in flight" = it ran
// within the last 300ms. Menus/pause (sim halted) fall back to the menu set.
std::atomic<int64_t> g_last_flight_ms{-1000000};

bool FlightActive() {
  return NowMs() - g_last_flight_ms.load(std::memory_order_relaxed) < 300;
}

// ---- Cursor hiding while steering -------------------------------------------
// The OS cursor is pinned to the window center during capture, so it must be
// hidden. Cursor visibility is decided by the window's WM_SETCURSOR handling
// on ITS thread, so the game window is subclassed (same-process, legal) and
// WM_SETCURSOR is answered with SetCursor(NULL) while capture is active.
#if defined(_WIN32)
std::atomic<bool> g_hide_cursor{false};
WNDPROC g_orig_wndproc = nullptr;
HWND g_subclassed_hwnd = nullptr;

// Free-cursor idle hide (see CursorIdleHideTick below): hidden after
// ac6_cursor_hide_seconds without motion, revealed on any real mouse input.
std::atomic<bool> g_idle_hidden{false};
std::atomic<int64_t> g_idle_last_motion_ms{0};

LRESULT CALLBACK KbmWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SETCURSOR && g_hide_cursor.load(std::memory_order_relaxed)) {
    SetCursor(nullptr);
    return TRUE;
  }
  if (msg == WM_MOUSEMOVE) {
    // WM_MOUSEMOVE is also sent for window-management reasons with the
    // cursor unmoved (see the SDK's note in window_win.cpp), so only a
    // POSITION CHANGE counts as real motion. Window-thread-only state.
    static LPARAM s_last_move_lp = -1;
    if (lp != s_last_move_lp) {
      s_last_move_lp = lp;
      if (g_idle_hidden.load(std::memory_order_relaxed)) {
        // Restamp the idle clock so the poll tick cannot immediately
        // re-hide. A motion event's own WM_SETCURSOR is sent BEFORE its
        // WM_MOUSEMOVE lands (still hidden then), so nudge a fresh one -
        // the arrow returns on THIS motion, not the next.
        g_idle_last_motion_ms.store(NowMs(), std::memory_order_relaxed);
        g_idle_hidden.store(false, std::memory_order_relaxed);
        PostMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), HTCLIENT);
      }
    }
  } else if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
             msg == WM_XBUTTONDOWN || msg == WM_MOUSEWHEEL) {
    if (g_idle_hidden.load(std::memory_order_relaxed)) {
      g_idle_last_motion_ms.store(NowMs(), std::memory_order_relaxed);
      g_idle_hidden.store(false, std::memory_order_relaxed);
      PostMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), HTCLIENT);
    }
  } else if (msg == WM_SETCURSOR && g_idle_hidden.load(std::memory_order_relaxed) &&
             LOWORD(lp) == HTCLIENT) {
    // Keep the cursor away over the client area while idle-hidden (window
    // management re-sets it otherwise). Non-client keeps normal arrows.
    SetCursor(nullptr);
    return TRUE;
  }
  return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

void EnsureCursorSubclass(HWND hwnd) {
  if (hwnd == g_subclassed_hwnd || hwnd == nullptr) return;
  WNDPROC prev = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&KbmWndProc)));
  if (prev != nullptr) {
    g_orig_wndproc = prev;
    g_subclassed_hwnd = hwnd;
  }
}

void SetCursorHidden(bool hide, HWND hwnd) {
  if (hide) EnsureCursorSubclass(hwnd);
  if (g_hide_cursor.exchange(hide, std::memory_order_relaxed) != hide && g_subclassed_hwnd) {
    // Nudge the window to re-evaluate the cursor immediately.
    PostMessageW(g_subclassed_hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(g_subclassed_hwnd),
                 HTCLIENT);
    if (hide) SetCursor(nullptr);
  }
}

// ---- Free-cursor idle hide --------------------------------------------------
// The FREE cursor (no steering capture) disappears after
// ac6_cursor_hide_seconds without motion and returns instantly on motion or
// a button - menus and desktop-style contexts; the capture hiding above is
// untouched. Never hides while an overlay is visible under the free cursor
// (a vanishing pointer over the F4 menu is an anti-feature), while the
// window is unfocused, or while capture owns the cursor. Runs on the input
// poll; the subclass proc above answers WM_SETCURSOR while hidden and
// reveals on real mouse input. Works for pad users too - not gated on
// ac6_kbm_enabled.
void CursorIdleHideTick() {
  const double secs = REXCVAR_GET(ac6_cursor_hide_seconds);
  const int64_t now = NowMs();
  bool want_hide = false;
  if (secs > 0.0 && !g_hide_cursor.load(std::memory_order_relaxed)) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == GetCurrentProcessId()) {
      EnsureCursorSubclass(fg);
      POINT p{};
      if (GetCursorPos(&p)) {
        static POINT s_last{};  // poll-thread only
        static bool s_have_last = false;
        if (!s_have_last || p.x != s_last.x || p.y != s_last.y) {
          s_have_last = true;
          s_last = p;
          g_idle_last_motion_ms.store(now, std::memory_order_relaxed);
        } else if (!rex::ui::ImGuiDrawer::DialogsVisible() &&
                   now - g_idle_last_motion_ms.load(std::memory_order_relaxed) >=
                       static_cast<int64_t>(secs * 1000.0)) {
          want_hide = true;
        }
      }
    }
  }
  if (g_idle_hidden.exchange(want_hide, std::memory_order_relaxed) != want_hide &&
      g_subclassed_hwnd) {
    // Nudge the window thread to apply the new state (hide, or re-arrow
    // after an overlay opened / the cvar changed - no mouse motion needed).
    PostMessageW(g_subclassed_hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(g_subclassed_hwnd),
                 HTCLIENT);
  }
}
#else
void SetCursorHidden(bool, void*) {}
void CursorIdleHideTick() {}
#endif

// ---- Mouse steering (M3) -----------------------------------------------------
// Virtual left stick fed by raw cursor deltas (state in MouseSteer above).
// While flying with mode=steer and the mouse owned by the game, the OS cursor
// is pinned to the game window's center each poll and the deltas accumulate
// into a clamped stick position with config-driven feel. Keyboard pitch/roll
// overrides its axis.
void MouseSteerRelease() {
  g_mouse.capturing = false;
  g_mouse.x = g_mouse.y = 0.0;
  g_mouse.rate_x = g_mouse.rate_y = 0.0;
  g_mouse.cam_x = g_mouse.cam_y = 0.0;
#if defined(_WIN32)
  SetCursorHidden(false, nullptr);
#endif
}

// Returns stick values in [-1,1]; false = steering inactive this poll.
// cam_mode (the held camera-control key) reroutes the mouse to a virtual
// RIGHT stick with absolute-position behavior: deflection accumulates and
// HOLDS while the mouse rests (the camera stays where you put it), and
// resets to neutral when the key is released.
bool MouseSteerPoll(bool cam_mode, double& out_x, double& out_y) {
#if defined(_WIN32)
  const MouseConfig& mc = g_config.mouse;
  if (mc.mode != "steer") {
    MouseSteerRelease();
    return false;
  }
  HWND fg = GetForegroundWindow();
  if (!fg) {
    MouseSteerRelease();
    return false;
  }
  RECT rc;
  if (!GetClientRect(fg, &rc)) {
    MouseSteerRelease();
    return false;
  }
  POINT center{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
  ClientToScreen(fg, &center);

  POINT cur;
  GetCursorPos(&cur);
  const int64_t now = NowMs();
  if (g_mouse.capturing && now - g_mouse.last_ms > 250) {
    g_mouse.capturing = false;  // stale anchor (gate was closed) - re-anchor
  }
  if (g_mouse.capturing) {
    const double dt = std::max(0.001, std::min(0.1, (now - g_mouse.last_ms) / 1000.0));
    double dx_px = static_cast<double>(cur.x - center.x);
    double dy_px = static_cast<double>(cur.y - center.y);
    if (mc.invert_x) dx_px = -dx_px;
    if (mc.invert_y) dy_px = -dy_px;

    if (cam_mode) {
      // Camera vertical is steering-invert-independent: mouse down always
      // looks down (negative = camera_down), whatever invert_y says.
      g_mouse.cam_x = std::max(
          -1.0, std::min(1.0, g_mouse.cam_x + dx_px * mc.sensitivity_x / 500.0));
      g_mouse.cam_y = std::max(
          -1.0, std::min(1.0, g_mouse.cam_y + (mc.invert_y ? dy_px : -dy_px) *
                                                  mc.sensitivity_y / 500.0));
      // Freeze the steering models while panning so releasing the camera key
      // never produces a stale steering kick.
      g_mouse.rate_x = g_mouse.rate_y = 0.0;
    } else if (mc.steer_model == "position") {
      const double dx = dx_px * mc.sensitivity_x / 500.0;
      const double dy = dy_px * mc.sensitivity_y / 500.0;
      g_mouse.x += dx;
      g_mouse.y += dy;  // screen-down positive = pull
      if (mc.recenter > 0.0 && dx == 0.0 && dy == 0.0) {
        const double decay = mc.recenter * dt;
        auto toward_zero = [&](double v) {
          if (v > decay) return v - decay;
          if (v < -decay) return v + decay;
          return 0.0;
        };
        g_mouse.x = toward_zero(g_mouse.x);
        g_mouse.y = toward_zero(g_mouse.y);
      }
      g_mouse.x = std::max(-1.0, std::min(1.0, g_mouse.x));
      g_mouse.y = std::max(-1.0, std::min(1.0, g_mouse.y));
    } else {
      // Velocity model (ACAH/AC7 style): deflection follows the CURRENT mouse
      // speed through a short EMA jitter filter, so it responds within a
      // frame and self-damps to zero the moment the mouse rests.
      const double inst_x = dx_px / dt;  // px/s this poll
      const double inst_y = dy_px / dt;
      const double alpha =
          mc.smoothing <= 0.0 ? 1.0 : 1.0 - std::exp(-dt / mc.smoothing);
      g_mouse.rate_x += (inst_x - g_mouse.rate_x) * alpha;
      g_mouse.rate_y += (inst_y - g_mouse.rate_y) * alpha;
      // The EMA decays asymptotically and never mathematically reaches zero,
      // so after one mouse twitch the residual would keep ownership of the
      // stick forever (the anti-deadzone re-expands even 1e-6 to a real
      // deflection, clobbering the physical pad's left stick). Below a few
      // px/s the mouse is at rest: snap to exact zero so the pad passes
      // through. Real motion is tens to hundreds of px/s.
      if (std::abs(g_mouse.rate_x) < 2.0) g_mouse.rate_x = 0.0;
      if (std::abs(g_mouse.rate_y) < 2.0) g_mouse.rate_y = 0.0;
      g_mouse.x = std::max(-1.0, std::min(1.0, g_mouse.rate_x * mc.sensitivity_x / 1000.0));
      g_mouse.y = std::max(-1.0, std::min(1.0, g_mouse.rate_y * mc.sensitivity_y / 1000.0));
    }
  } else {
    g_mouse.capturing = true;  // first poll: no delta, just take the anchor
  }
  if (!cam_mode) {
    // Releasing the camera-control key returns the camera to neutral.
    g_mouse.cam_x = g_mouse.cam_y = 0.0;
  }
  g_mouse.last_ms = now;
  SetCursorPos(center.x, center.y);
  SetCursorHidden(true, fg);

  auto shape = [&](double v) {
    double a = std::abs(v);
    if (a <= mc.deadzone) return 0.0;
    a = (a - mc.deadzone) / (1.0 - mc.deadzone);
    if (mc.curve_exponent != 1.0) a = std::pow(a, mc.curve_exponent);
    // Anti-deadzone: skip past the engine's own stick deadzone so small
    // motions respond immediately.
    if (mc.anti_deadzone > 0.0) a = mc.anti_deadzone + a * (1.0 - mc.anti_deadzone);
    if (a > 1.0) a = 1.0;
    return v < 0 ? -a : a;
  };
  out_x = shape(cam_mode ? g_mouse.cam_x : g_mouse.x);
  out_y = shape(cam_mode ? g_mouse.cam_y : g_mouse.y);
  return true;
#else
  (void)cam_mode;
  (void)out_x;
  (void)out_y;
  return false;
#endif
}

// ---- Per-instance injection state -------------------------------------------
struct InstanceState {
  uint32_t prev = 0;             // our injected level bits last frame
  float elapsed[32] = {};        // auto-repeat timers (game-dt units)
  float threshold[32] = {};      // current repeat threshold per bit
};
std::unordered_map<uint32_t, InstanceState> g_state;  // guest inst addr -> state

uint32_t LoadWord(uint8_t* base, uint32_t ea) {
  return rex::memory::load_and_swap<uint32_t>(base + ea);
}
void StoreWord(uint8_t* base, uint32_t ea, uint32_t v) {
  rex::memory::store_and_swap<uint32_t>(base + ea, v);
}

// One u32 mask of mirror-space button bits per action slot, per device block.
// Device block 0 (pad 0) is representative; the game applies the same layout
// to all four.
uint32_t ActionMask(uint8_t* base, uint32_t inst, int action) {
  return LoadWord(base, inst + 4 + 8 + static_cast<uint32_t>(action) * 4);
}

// One-time dump of every instance's nonzero binding masks: the definitive
// per-context action tables (also the M2/M3 planning data).
void DumpMaskTables(uint8_t* base, uint32_t singleton, uint32_t fifth) {
  if (!REXCVAR_GET(ac6_kbm_log)) return;
  for (int k = 0; k < 5; ++k) {
    const uint32_t inst = (k < 4) ? singleton + kInstanceOffsets[k] : fifth;
    if (inst == 0) continue;
    std::string s;
    for (int a = 0; a < 32; ++a) {
      const uint32_t m = ActionMask(base, inst, a);
      if (m) s += fmt::format("a{}=0x{:X} ", a, m);
    }
    const uint32_t analog_en = LoadWord(base, inst + 4 + 0x88);
    const uint32_t invert = LoadWord(base, inst + 4 + 0x8C);
    KbmLog(fmt::format("masks inst[{}]@0x{:08X}: {} analogEn=0x{:X} invert=0x{:X}", k, inst, s,
                       analog_en, invert));
  }
}

uint32_t GatherMirrorBits(const GateState& gate) {
  uint32_t mirror = 0;
  for (size_t i = 0; i < kNumMenuActions; ++i) {
    for (VirtualKey vk : g_config.menu_keys[i]) {
      if (KeyAllowed(gate, vk) && KeyHeld(vk)) {
        mirror |= 1u << kMenuActions[i].mirror_bit;
        break;
      }
    }
  }
  return mirror;
}

void InjectMenu(uint8_t* base, uint32_t inst, double dt, uint32_t kb_mirror) {
  // Translate mirror-space presses into THIS instance's action bits via its
  // own binding masks - bit-exact with what a real pad press produces here.
  // kb_mirror is already input-ownership-gated by GatherMirrorBits; a gate
  // closing mid-hold arrives as kb_mirror dropping to 0, which produces the
  // released-edge below - keys never stick when an overlay takes the input.
  uint32_t level = 0;
  if (kb_mirror) {
    for (int a = 0; a < 32; ++a) {
      if (ActionMask(base, inst, a) & kb_mirror) {
        level |= 1u << a;
      }
    }
  }

  InstanceState& st = g_state[inst];
  const uint32_t pressed = level & ~st.prev;
  const uint32_t released = st.prev & ~level;
  uint32_t repeat_fire = pressed;

  if (level != 0 || st.prev != 0) {
    // Auto-repeat with the instance's own tuning (identical feel to the pad).
    const float delay = rex::memory::load_and_swap<float>(base + inst + kOffRepeatDelay);
    const float rate = rex::memory::load_and_swap<float>(base + inst + kOffRepeatRate);
    float fdt = static_cast<float>(dt);
    if (!(fdt >= 0.0f)) fdt = 0.0f;  // also catches NaN
    for (int b = 0; b < 32; ++b) {
      const uint32_t bit = 1u << b;
      if (pressed & bit) {
        st.elapsed[b] = 0.0f;
        st.threshold[b] = delay;
      } else if (level & bit) {
        st.elapsed[b] += fdt;
        if (st.elapsed[b] >= st.threshold[b]) {
          repeat_fire |= bit;
          st.elapsed[b] = 0.0f;
          st.threshold[b] = rate;
        }
      }
    }
  }

  if (level | pressed | released | repeat_fire) {
    StoreWord(base, inst + kOffLevel, LoadWord(base, inst + kOffLevel) | level);
    StoreWord(base, inst + kOffPressed, LoadWord(base, inst + kOffPressed) | pressed);
    // Mask the spurious released-edge the game computes while our key is
    // still held (our bit was in its prev but not in its pad-only level),
    // then add our genuine releases.
    uint32_t rel = LoadWord(base, inst + kOffReleased);
    rel = (rel & ~level) | released;
    StoreWord(base, inst + kOffReleased, rel);
    StoreWord(base, inst + kOffRepeat, LoadWord(base, inst + kOffRepeat) | repeat_fire);

    if (pressed && REXCVAR_GET(ac6_kbm_log)) {
      static int s_lines = 0;
      if (s_lines < 200) {
        ++s_lines;
        KbmLog(fmt::format("inject: mirror 0x{:X} -> level 0x{:X} inst=0x{:08X}", kb_mirror,
                           level, inst));
      }
    }
  }
  st.prev = level;
}

// ---- Layer watch: trace pad data bottom-up through the input stack ----------
// Round-3 finding: real pad presses (which visibly drove the front-end) never
// reached ANY CGaUserInput instance. Watch each layer's buttons word so one
// run shows where the data stops per screen:
//   NU devices (fixed):  0x8290DDBC/0x8290DE44/0x8290DECC/0x8290DF54, +0x1C level
//   galib mirror:        0x826EDB18 + i*0xA0, +8 buttons
constexpr uint32_t kDeviceAddrs[4] = {0x8290DDBC, 0x8290DE44, 0x8290DECC, 0x8290DF54};
constexpr uint32_t kMirrorBase = 0x826EDB18;

void WatchLayers(uint8_t* base) {
  if (!REXCVAR_GET(ac6_kbm_log)) return;
  static uint32_t s_dev[4] = {};
  static uint32_t s_mir[4] = {};
  static int s_lines = 0;
  if (s_lines >= 600) return;
  for (int i = 0; i < 4; ++i) {
    const uint32_t dev = LoadWord(base, kDeviceAddrs[i] + 0x1C);
    if (dev != s_dev[i]) {
      s_dev[i] = dev;
      ++s_lines;
      KbmLog(fmt::format("watch dev[{}] buttons 0x{:08X}", i, dev));
    }
    const uint32_t mir = LoadWord(base, kMirrorBase + i * 0xA0 + 8);
    if (mir != s_mir[i]) {
      s_mir[i] = mir;
      ++s_lines;
      KbmLog(fmt::format("watch mirror[{}] buttons 0x{:08X}", i, mir));
    }
  }
}

// ---- Probe: log game-side word changes + flight analog activity -------------
// Answers the M0 open questions from one normal play session:
//  - which bits the pad sets per action (press pad buttons -> bit map)
//  - which instances screens consume (navigate while watching)
//  - which analog slots the sticks drive (move sticks in flight)
void Probe(uint8_t* base, uint32_t singleton, uint32_t inst) {
  if (!REXCVAR_GET(ac6_kbm_log)) return;

  int idx = 4;  // non-singleton = the pump's fifth instance
  for (int i = 0; i < 4; ++i) {
    if (inst == singleton + kInstanceOffsets[i]) idx = i;
  }

  static std::unordered_map<uint32_t, uint32_t> s_last_level;
  const uint32_t level = LoadWord(base, inst + kOffLevel);
  auto it = s_last_level.find(inst);
  if (it == s_last_level.end() || it->second != level) {
    s_last_level[inst] = level;
    static int s_lines = 0;
    if (s_lines < 400) {  // hard cap so a stuck bit can't flood the log
      ++s_lines;
      KbmLog(fmt::format("probe inst[{}] level 0x{:08X}", idx, level));
    }
  }

  // Analog probe: flight instance only, at most 2 lines/second.
  if (idx == 2) {
    using clock = std::chrono::steady_clock;
    static clock::time_point s_next = clock::now();
    const auto now = clock::now();
    if (now >= s_next) {
      std::string a, c;
      for (int i = 0; i < 32; ++i) {
        const float va = rex::memory::load_and_swap<float>(base + inst + kOffAnalogA + i * 4);
        const float vc = rex::memory::load_and_swap<float>(base + inst + kOffAnalogC + i * 4);
        if (va > 0.2f || va < -0.2f) a += fmt::format("{}:{:+.2f} ", i, va);
        if (vc > 0.2f || vc < -0.2f) c += fmt::format("{}:{:+.2f} ", i, vc);
      }
      if (!a.empty() || !c.empty()) {
        s_next = now + std::chrono::milliseconds(500);
        KbmLog(fmt::format("probe inst[2] analogA[{}] analogC[{}]", a, c));
      }
    }
  }
}

}  // namespace

PPC_EXTERN_FUNC(__imp__rex_sub_82390CE0);  // guest XamInputGetState(user,0,state) wrapper

// Round-5 verdict: action-word injection is bit-exact with the pad's values
// yet front-end screens ignore it - they consume a LOWER layer (the NU device
// state the pad poll fills). So the menu key set is additionally injected
// right here at the XamInputGetState boundary as XINPUT button bits: every
// downstream layer then sees keyboard presses exactly as pad presses.
// (Flight/M2 will gate this by game mode so flight keys never collide.)
uint32_t GatherXInputBits(const GateState& gate) {
  static const uint16_t kXBits[kNumMenuActions] = {
      0x0001,  // up      -> DPAD_UP
      0x0002,  // down    -> DPAD_DOWN
      0x0004,  // left    -> DPAD_LEFT
      0x0008,  // right   -> DPAD_RIGHT
      0x1000,  // confirm -> A
      0x2000,  // cancel  -> B
      0x0010,  // start   -> START
      0x0020,  // back    -> BACK
  };
  uint32_t bits = 0;
  for (size_t i = 0; i < kNumMenuActions; ++i) {
    for (VirtualKey vk : g_config.menu_keys[i]) {
      if (KeyAllowed(gate, vk) && KeyHeld(vk)) {
        bits |= kXBits[i];
        break;
      }
    }
  }
  return bits;
}

PPC_FUNC_IMPL(rex_sub_82390CE0) {
  PPC_FUNC_PROLOGUE();

  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t user = ctx.r3.u32;
  const uint32_t state_ptr = ctx.r4.u32;
  __imp__rex_sub_82390CE0(ctx, base);

  // Free-cursor idle hide rides the input poll (user 0 = once per poll
  // round). Deliberately outside the ac6_kbm_enabled gate: pad users get
  // the timeout too.
  if (user == 0) {
    CursorIdleHideTick();
  }

  // Pad-less operation: when no controller is connected (0x48F), present a
  // neutral synthetic pad on slot 0; the injection below then supplies the
  // keyboard/mouse state exactly as if a pad were plugged in.
  if (REXCVAR_GET(ac6_kbm_enabled) && REXCVAR_GET(ac6_kbm_padless) && user == 0 &&
      state_ptr != 0 && ctx.r3.u32 == 0x48F) {
    static uint32_t s_packet = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      *(base + state_ptr + i) = 0;
    }
    rex::memory::store_and_swap<uint32_t>(base + state_ptr + 0, ++s_packet);
    ctx.r3.u64 = 0;
  }

  // Device-level keyboard injection (user 0 only, successful polls only).
  // Context switch: flying -> [flight] key set; everything else -> [menu] set.
  // Ownership gating is per input KIND (KeyAllowed): keyboard keys drop out
  // while an overlay holds keyboard focus, mouse buttons and the wheel while
  // the cursor is over an overlay. A gate closing mid-hold simply stops
  // asserting the bits - downstream edge detection sees a clean release.
  if (REXCVAR_GET(ac6_kbm_enabled) && user == 0 && state_ptr != 0 &&
      ctx.r3.u32 == 0) {
    const GateState gate = QueryGate();
    if (FlightActive()) {
      uint16_t btn = 0;
      uint8_t lt = 0, rt = 0;
      int32_t lx = 0, ly = 0, rx = 0, ry = 0;
      for (size_t i = 0; i < kNumFlightActions; ++i) {
        for (VirtualKey vk : g_config.flight_keys[i]) {
          if (KeyAllowed(gate, vk) && KeyHeld(vk)) {
            const FlightActionDef& d = kFlightActions[i];
            btn |= d.buttons;
            if (d.lt > lt) lt = d.lt;
            if (d.rt > rt) rt = d.rt;
            lx += d.lx;
            ly += d.ly;
            rx += d.rx;
            ry += d.ry;
            break;
          }
        }
      }
      // AC7-style camera control key: while held, the mouse drives the
      // camera (right stick) instead of pitch/roll.
      bool cam_mode = false;
      for (VirtualKey vk : g_config.flight_keys[CameraControlAction()]) {
        if (KeyAllowed(gate, vk) && KeyHeld(vk)) {
          cam_mode = true;
          break;
        }
      }

      // Mouse steering: runs every flight poll while the game owns the
      // pointer (maintains capture/recenter); keyboard keys override the
      // mouse per axis, pad stick wins when both idle. With the camera key
      // held, mx/my are the held camera deflection instead. If a visible
      // overlay owns the pointer (cursor hovering it while free), steering
      // does not acquire - the cursor stays live for the overlay until it
      // moves off, exactly like a desktop window under the pointer.
      double mx = 0.0, my = 0.0;
      bool mouse_on = false;
      if (gate.mouse_ok()) {
        mouse_on = MouseSteerPoll(cam_mode, mx, my);
      } else {
        MouseSteerRelease();
      }

      const bool any = btn || lt || rt || lx != 0 || ly != 0 || rx != 0 || ry != 0 ||
                       (mouse_on && (mx != 0.0 || my != 0.0));
      if (any) {
        const uint16_t cur = rex::memory::load_and_swap<uint16_t>(base + state_ptr + 4);
        rex::memory::store_and_swap<uint16_t>(base + state_ptr + 4,
                                              static_cast<uint16_t>(cur | btn));
        auto max_u8 = [&](uint32_t off, uint8_t v) {
          uint8_t c = *(base + state_ptr + off);
          if (v > c) *(base + state_ptr + off) = v;
        };
        max_u8(6, lt);
        max_u8(7, rt);
        auto clamp16 = [](int32_t v) {
          return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
        };
        if (lx != 0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 8, clamp16(lx));
        } else if (mouse_on && !cam_mode && mx != 0.0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 8,
                                               clamp16(static_cast<int32_t>(mx * 32767.0)));
        }
        // Mouse "pull" (my positive) = stick pulled = negative LY, matching
        // the pitch_up fallback key.
        if (ly != 0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 10, clamp16(ly));
        } else if (mouse_on && !cam_mode && my != 0.0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 10,
                                               clamp16(static_cast<int32_t>(-my * 32767.0)));
        }
        if (rx != 0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 12, clamp16(rx));
        } else if (mouse_on && cam_mode && mx != 0.0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 12,
                                               clamp16(static_cast<int32_t>(mx * 32767.0)));
        }
        if (ry != 0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 14, clamp16(ry));
        } else if (mouse_on && cam_mode && my != 0.0) {
          rex::memory::store_and_swap<int16_t>(base + state_ptr + 14,
                                               clamp16(static_cast<int32_t>(my * 32767.0)));
        }
        static int s_lines = 0;
        static uint32_t s_last = 0;
        const uint32_t sig = btn | (lt << 16) | (rt << 24) | ((lx != 0) << 30) |
                             ((ly != 0) << 31);
        if (REXCVAR_GET(ac6_kbm_log) && sig != s_last && s_lines < 100) {
          s_last = sig;
          ++s_lines;
          KbmLog(fmt::format("xinput inject FLIGHT btn=0x{:04X} lt={} rt={} lx={} ly={} "
                             "mouse={} cam={} mx={:.2f} my={:.2f}",
                             btn, lt, rt, lx, ly, mouse_on, cam_mode, mx, my));
        }
      }
    } else {
      MouseSteerRelease();
      const uint32_t kb = GatherXInputBits(gate);
      if (kb != 0) {
        const uint16_t cur = rex::memory::load_and_swap<uint16_t>(base + state_ptr + 4);
        rex::memory::store_and_swap<uint16_t>(base + state_ptr + 4,
                                              static_cast<uint16_t>(cur | kb));
        static int s_lines = 0;
        static uint32_t s_last = 0;
        if (REXCVAR_GET(ac6_kbm_log) && kb != s_last && s_lines < 100) {
          s_last = kb;
          ++s_lines;
          KbmLog(fmt::format("xinput inject wButtons 0x{:04X}", kb));
        }
      }
    }
  }

  if (!REXCVAR_GET(ac6_kbm_log)) {
    return;
  }
  static uint32_t s_callers[8] = {};
  static int s_ncallers = 0;
  bool known = false;
  for (int i = 0; i < s_ncallers; ++i) {
    if (s_callers[i] == lr) known = true;
  }
  if (!known && s_ncallers < 8) {
    s_callers[s_ncallers++] = lr;
    KbmLog(fmt::format("xinput poll site lr=0x{:08X} user={} ret=0x{:X}", lr, user, ctx.r3.u32));
  }
  if (state_ptr != 0 && ctx.r3.u32 == 0) {
    const uint16_t buttons = rex::memory::load_and_swap<uint16_t>(base + state_ptr + 4);
    static uint16_t s_last[4] = {};
    static int s_lines = 0;
    if (user < 4 && buttons != s_last[user] && s_lines < 300) {
      s_last[user] = buttons;
      ++s_lines;
      KbmLog(fmt::format("xinput user={} wButtons 0x{:04X} (lr=0x{:08X})", user, buttons, lr));
    }
  }
}

// Primary flight heartbeat, called from the fps-physics wrappers in
// ac6_fps_physics_fix.cpp: the main force step rex_sub_823046A0 (airborne),
// the alternate step rex_sub_82305278 (rolling on the ground, mode bit 0x40),
// and - player-scoped - the master updates plus the ground-state maintainer,
// which still tick at a full standstill where the deactivated model (bit
// 0x80) steps neither. All of them halt when the sim halts, so menus and
// pause still fall back to the menu key set. The sampler hook below turned
// out to be control-mode dependent (never fired in the M2 round-1 field
// test), so it stays only as a secondary signal.
void ac6KbmNotifyFlightStep() {
  g_last_flight_ms.store(NowMs(), std::memory_order_relaxed);
}

// Hands the module the application window. Windows polls the OS for key state
// and needs nothing here; every other platform has no such poll, so key, mouse
// button and wheel state is accumulated from this window's input events.
// Called once, after the window exists. Safe to call when kbm is disabled: the
// listener only maintains state, and every consumer is behind
// ac6_kbm_enabled.
void ac6KbmAttachWindow([[maybe_unused]] rex::ui::Window* window) {
#if !defined(_WIN32)
  if (!window || g_window_attached.load(std::memory_order_relaxed)) {
    return;
  }
  // z-order 0 matches the application's own listener: the overlays sit above
  // and the gate in QueryGate is what actually yields input to them, so this
  // must not sit above the overlays and swallow their keys.
  window->AddInputListener(&g_window_input, 0);
  window->AddListener(&g_window_input);
  g_window_attached.store(true, std::memory_order_relaxed);
  KbmLog("window input attached (keyboard, mouse buttons and wheel)");
#endif
}

PPC_EXTERN_FUNC(__imp__sub_82390CD8);  // guest XamInputGetCapabilities wrapper

// Pad-less: report a standard gamepad on slot 0 when the real query fails,
// so the NU device layer completes its connect handshake without hardware.
PPC_FUNC_IMPL(sub_82390CD8) {
  PPC_FUNC_PROLOGUE();
  const uint32_t user = ctx.r3.u32;
  const uint32_t caps = ctx.r5.u32;
  __imp__sub_82390CD8(ctx, base);
  if (REXCVAR_GET(ac6_kbm_enabled) && REXCVAR_GET(ac6_kbm_padless) && user == 0 && caps != 0 &&
      ctx.r3.u32 != 0) {
    *(base + caps + 0) = 1;  // XINPUT_DEVTYPE_GAMEPAD
    *(base + caps + 1) = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    rex::memory::store_and_swap<uint16_t>(base + caps + 2, 0);
    for (uint32_t i = 4; i < 20; ++i) {
      *(base + caps + i) = 0xFF;  // full gamepad + vibration capability bits
    }
    ctx.r3.u64 = 0;
    if (REXCVAR_GET(ac6_kbm_log)) {
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        KbmLog("padless: synthetic gamepad reported on slot 0");
      }
    }
  }
}

PPC_EXTERN_FUNC(__imp__rex_sub_82390CF0);  // guest XamInputSetState (vibration) wrapper

// Pad-less: swallow vibration failures for slot 0 so the synthetic pad never
// churns through reconnects.
PPC_FUNC_IMPL(rex_sub_82390CF0) {
  PPC_FUNC_PROLOGUE();
  const uint32_t user = ctx.r3.u32;
  __imp__rex_sub_82390CF0(ctx, base);
  if (REXCVAR_GET(ac6_kbm_enabled) && REXCVAR_GET(ac6_kbm_padless) && user == 0 &&
      ctx.r3.u32 != 0) {
    ctx.r3.u64 = 0;
  }
}

PPC_EXTERN_FUNC(__imp__rex_sub_82191AE8);  // player flight-control sampler

// Pass-through timestamp: secondary proof-of-flight for the context switch.
PPC_FUNC_IMPL(rex_sub_82191AE8) {
  PPC_FUNC_PROLOGUE();
  g_last_flight_ms.store(NowMs(), std::memory_order_relaxed);
  __imp__rex_sub_82191AE8(ctx, base);
}

PPC_EXTERN_FUNC(__imp__rex_sub_82211E28);  // galib::CGaUserInput::Update

PPC_FUNC_IMPL(rex_sub_82211E28) {
  PPC_FUNC_PROLOGUE();

  const uint32_t inst = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__rex_sub_82211E28(ctx, base);

  if (!REXCVAR_GET(ac6_kbm_enabled) || inst == 0) {
    return;
  }

  // One-time init + ~1x/second hot reload (the pump calls this 5x per frame).
  const uint32_t call = g_hook_calls++;
  if (call == 0) {
    LoadConfig();
    EnsureWheelThread();
    EnforceMnkOff();
  } else if ((call % 300) == 0) {
    MaybeReloadConfig();
    EnforceMnkOff();
  }

  const uint32_t singleton = LoadWord(base, kSingletonPtrGlobal);
  if (singleton == 0) {
    return;
  }

  const GateState gate = QueryGate();

  // The pump updates the 4 singleton instances plus one it owns itself; any
  // Update call on a non-singleton instance is that fifth one ("instance 4").
  static uint32_t s_fifth_inst = 0;
  int idx = -1;
  for (int i = 0; i < 4; ++i) {
    if (inst == singleton + kInstanceOffsets[i]) idx = i;
  }
  if (idx < 0) {
    s_fifth_inst = inst;
    idx = 4;
  }

  // One-time mask-table dump once the fifth instance is known.
  static bool s_dumped = false;
  if (!s_dumped && s_fifth_inst != 0 && call > 5) {
    s_dumped = true;
    DumpMaskTables(base, singleton, s_fifth_inst);
  }

  const uint32_t kb_mirror = GatherMirrorBits(gate);

  // Gate transitions (capped): one line whenever input ownership changes -
  // the direct log evidence for the overlay open/close and hover checks.
  if (REXCVAR_GET(ac6_kbm_log)) {
    static int s_gate_lines = 0;
    static uint32_t s_gate_last = 0xFF;
    const uint32_t sig = (gate.fg_ok << 0) | (gate.capture_mouse << 1) |
                         (gate.capture_keyboard << 2) | (gate.want_text << 3) |
                         (gate.steer_owns_pointer << 4);
    if (sig != s_gate_last && s_gate_lines < 200) {
      s_gate_last = sig;
      ++s_gate_lines;
      KbmLog(fmt::format(
          "gate change: fg={} capMouse={} capKb={} text={} steerOwn={} -> keys={} mouse={}",
          gate.fg_ok, gate.capture_mouse, gate.capture_keyboard, gate.want_text,
          gate.steer_owns_pointer, gate.keys_ok(), gate.mouse_ok()));
    }
  }

  // Flight-context transitions. While FlightActive() is false the flight
  // injection below is skipped entirely, so the guest keeps whatever stick
  // values were last written - the controls appear frozen at the last input.
  // This traces exactly when that happens and for how long, which separates
  // "the gate closed" from "the flight sim stopped stepping".
  if (REXCVAR_GET(ac6_kbm_log)) {
    static bool s_last_flight = false;
    static int64_t s_flight_since = 0;
    static int s_flight_lines = 0;
    const bool flight_now = FlightActive();
    if (flight_now != s_last_flight && s_flight_lines < 400) {
      const int64_t now = NowMs();
      ++s_flight_lines;
      KbmLog(fmt::format(
          "flight context -> {} after {}ms as {}; flight stick injection {}",
          flight_now ? "IN-FLIGHT" : "NOT-IN-FLIGHT", s_flight_since ? now - s_flight_since : 0,
          s_last_flight ? "IN-FLIGHT" : "NOT-IN-FLIGHT", flight_now ? "RESUMES" : "STOPS"));
      s_last_flight = flight_now;
      s_flight_since = now;
    }
  }

  // Heartbeat: proves the hook runs, shows the gate, and shows raw key
  // detection INDEPENDENT of the gate (so a closed gate is visible too).
  if (REXCVAR_GET(ac6_kbm_log) && (call == 5 || (call % 3000) == 0)) {
    GateState raw;  // all-open gate: rawMirror = physical key detection
    raw.fg_ok = true;
    KbmLog(fmt::format(
        "alive call={} inst=0x{:08X} singleton=0x{:08X} fifth@0x{:08X} dt={:.4f} "
        "gate[fg={} capMouse={} capKb={} text={} steerOwn={} keys={} mouse={}] targets=[{}] "
        "rawMirror=0x{:X} gatedMirror=0x{:X} flight={}",
        call, inst, singleton, s_fifth_inst, dt, gate.fg_ok, gate.capture_mouse,
        gate.capture_keyboard, gate.want_text, gate.steer_owns_pointer, gate.keys_ok(),
        gate.mouse_ok(), fmt::join(g_config.menu_instances, ","), GatherMirrorBits(raw),
        kb_mirror, FlightActive()));
  }

  if (!FlightActive()) {
    for (int target : g_config.menu_instances) {
      if (target == idx && idx != 2) {
        InjectMenu(base, inst, dt, kb_mirror);
        break;
      }
    }
  }
  // Flight (instance[2]) injection lands in M2/M3.

  if (idx == 0) {
    WatchLayers(base);  // once per pump pass, not 5x
  }
  Probe(base, singleton, inst);
}
