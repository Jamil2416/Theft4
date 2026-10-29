// In-game trainer for the iOS app.
//
// The game registers every script command by name through sub_82845600(name,
// handler) at startup. That hook records the handlers this file uses and puts
// a host wrapper in front of WAIT, which every running game script calls each
// frame. The wrapper runs the trainer from inside that script thread, so
// commands that create entities or cameras have the thread context they
// expect, then runs the real WAIT. Calls use the game's own native context
// layout ({return storage, argument count, argument vector}) in a scratch
// frame below the script thread's stack pointer, the same way the desktop
// quicksave hooks call commands.
#include "theft4_trainer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "gta4_init.h"
#ifdef THEFT4_HAS_GTA4_NATIVE_BACKEND
#include "gta4_gameplay_mods.h"
#endif

namespace {

// ---------------------------------------------------------------------------
// Guest memory helpers
// ---------------------------------------------------------------------------

bool Span(uint8_t* base, uint32_t address, size_t size, bool write = false) {
  if (!base || !address || !size || uint64_t(address) + size > uint64_t(UINT32_MAX) + 1)
    return false;
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  const auto last = uint32_t(uint64_t(address) + size - 1);
  if (!heap || heap != memory->LookupHeap(last)) return false;
  const auto access = heap->QueryRangeAccess(address, last);
  using rex::memory::PageAccess;
  return access == PageAccess::kReadWrite || access == PageAccess::kExecuteReadWrite ||
         (!write && (access == PageAccess::kReadOnly || access == PageAccess::kExecuteReadOnly));
}
uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, rex::memory::GuestPtr(base, address), 4);
  return __builtin_bswap32(value);
}
void Write(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(rex::memory::GuestPtr(base, address), &value, 4);
}
std::string GuestString(uint8_t* base, uint32_t address, size_t cap = 64) {
  std::string out;
  for (size_t i = 0; i < cap; ++i) {
    if (!Span(base, address + uint32_t(i), 1)) return {};
    const char c = char(*rex::memory::GuestPtr(base, address + uint32_t(i)));
    if (!c) return out;
    out.push_back(c);
  }
  return {};
}
uint32_t F(float value) { return std::bit_cast<uint32_t>(value); }
float AsFloat(uint32_t bits) { return std::bit_cast<float>(bits); }

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// GTA IV's model/name hash (GET_HASH_KEY): Jenkins one-at-a-time over the
// lower-case name.
uint32_t ModelHash(std::string_view name) {
  uint32_t h = 0;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    h += uint8_t(c);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

// ---------------------------------------------------------------------------
// Script commands
// ---------------------------------------------------------------------------

enum class N : int {
  WAIT,
  GET_PLAYER_ID, IS_PLAYER_PLAYING, GET_PLAYER_CHAR, IS_CHAR_DEAD, IS_SCREEN_FADED_IN,
  SET_PLAYER_INVINCIBLE, SET_CHAR_PROOFS, SET_CHAR_SUFFERS_CRITICAL_HITS,
  SET_MAX_WANTED_LEVEL, CLEAR_WANTED_LEVEL, ALTER_WANTED_LEVEL, APPLY_WANTED_LEVEL_CHANGE_NOW,
  SET_PLAYER_NEVER_GETS_TIRED, SET_CHAR_HEALTH, ADD_ARMOUR_TO_CHAR, GET_CHAR_ARMOUR,
  SET_CHAR_VISIBLE, UNLOCK_RAGDOLL, SWITCH_PED_TO_RAGDOLL, SET_CHAR_CAN_BE_KNOCKED_OFF_BIKE,
  SET_CHAR_COLLISION, FREEZE_CHAR_POSITION, SET_CHAR_COORDINATES, SET_CHAR_COORDINATES_NO_OFFSET,
  GET_CHAR_COORDINATES, GET_CHAR_HEADING, SET_CHAR_HEADING,
  STORE_SCORE, ADD_SCORE,
  IS_CHAR_IN_ANY_CAR, STORE_CAR_CHAR_IS_IN_NO_SAVE, DOES_VEHICLE_EXIST,
  REQUEST_MODEL, HAS_MODEL_LOADED, IS_MODEL_IN_CDIMAGE, MARK_MODEL_AS_NO_LONGER_NEEDED,
  CREATE_CAR, WARP_CHAR_INTO_CAR, MARK_CAR_AS_NO_LONGER_NEEDED, DELETE_CAR,
  WARP_CHAR_FROM_CAR_TO_COORD,
  SET_CAR_CAN_BE_DAMAGED, SET_CAR_CAN_BE_VISIBLY_DAMAGED, SET_CAR_PROOFS, FIX_CAR,
  GET_CAR_HEALTH, SET_CAR_VISIBLE, EXPLODE_CAR, BURST_CAR_TYRE, SET_VEHICLE_DIRT_LEVEL,
  SET_CAR_ON_GROUND_PROPERLY, GET_CAR_SPEED, SET_CAR_FORWARD_SPEED, SET_CAR_COORDINATES,
  SET_CAR_HEADING, GET_CAR_HEADING,
  IS_PLAYER_PRESSING_HORN, IS_BUTTON_PRESSED, GET_POSITION_OF_ANALOGUE_STICKS,
  SET_TIME_OF_DAY, FORCE_TIME_OF_DAY, RELEASE_TIME_OF_DAY, FORCE_WEATHER_NOW, RELEASE_WEATHER,
  SET_CAR_DENSITY_MULTIPLIER, SET_PED_DENSITY_MULTIPLIER, SET_WANTED_MULTIPLIER,
  DISPLAY_HUD, DISPLAY_RADAR,
  GIVE_WEAPON_TO_CHAR, REMOVE_ALL_CHAR_WEAPONS,
  GET_FIRST_BLIP_INFO_ID, DOES_BLIP_EXIST, GET_BLIP_COORDS, GET_GROUND_Z_FOR_3D_COORD,
  CREATE_CAM, DESTROY_CAM, DOES_CAM_EXIST, SET_CAM_ACTIVE, SET_CAM_PROPAGATE,
  ACTIVATE_SCRIPTED_CAMS, SET_CAM_POS, SET_CAM_ROT, SET_CAM_FOV, GET_CAM_POS, GET_CAM_ROT,
  GET_GAME_CAM, SET_PLAYER_CONTROL,
  SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY, RETUNE_RADIO_UP, RETUNE_RADIO_DOWN,
  SKIP_RADIO_FORWARD,
  APPLY_FORCE_TO_CAR, GET_NTH_CLOSEST_CAR_NODE_WITH_HEADING, CREATE_RANDOM_CHAR_AS_DRIVER,
  CREATE_CHAR_INSIDE_CAR, TASK_CAR_DRIVE_WANDER, MARK_CHAR_AS_NO_LONGER_NEEDED, CAM_IS_SPHERE_VISIBLE,
  IS_CHAR_SHOOTING, GET_CURRENT_CHAR_WEAPON, ADD_EXPLOSION, GET_CLOSEST_CHAR, GET_CLOSEST_CAR,
  GET_CAR_COORDINATES,
  COUNT
};

constexpr std::array<std::string_view, size_t(N::COUNT)> kNames = {
  "WAIT",
  "GET_PLAYER_ID", "IS_PLAYER_PLAYING", "GET_PLAYER_CHAR", "IS_CHAR_DEAD", "IS_SCREEN_FADED_IN",
  "SET_PLAYER_INVINCIBLE", "SET_CHAR_PROOFS", "SET_CHAR_SUFFERS_CRITICAL_HITS",
  "SET_MAX_WANTED_LEVEL", "CLEAR_WANTED_LEVEL", "ALTER_WANTED_LEVEL", "APPLY_WANTED_LEVEL_CHANGE_NOW",
  "SET_PLAYER_NEVER_GETS_TIRED", "SET_CHAR_HEALTH", "ADD_ARMOUR_TO_CHAR", "GET_CHAR_ARMOUR",
  "SET_CHAR_VISIBLE", "UNLOCK_RAGDOLL", "SWITCH_PED_TO_RAGDOLL", "SET_CHAR_CAN_BE_KNOCKED_OFF_BIKE",
  "SET_CHAR_COLLISION", "FREEZE_CHAR_POSITION", "SET_CHAR_COORDINATES", "SET_CHAR_COORDINATES_NO_OFFSET",
  "GET_CHAR_COORDINATES", "GET_CHAR_HEADING", "SET_CHAR_HEADING",
  "STORE_SCORE", "ADD_SCORE",
  "IS_CHAR_IN_ANY_CAR", "STORE_CAR_CHAR_IS_IN_NO_SAVE", "DOES_VEHICLE_EXIST",
  "REQUEST_MODEL", "HAS_MODEL_LOADED", "IS_MODEL_IN_CDIMAGE", "MARK_MODEL_AS_NO_LONGER_NEEDED",
  "CREATE_CAR", "WARP_CHAR_INTO_CAR", "MARK_CAR_AS_NO_LONGER_NEEDED", "DELETE_CAR",
  "WARP_CHAR_FROM_CAR_TO_COORD",
  "SET_CAR_CAN_BE_DAMAGED", "SET_CAR_CAN_BE_VISIBLY_DAMAGED", "SET_CAR_PROOFS", "FIX_CAR",
  "GET_CAR_HEALTH", "SET_CAR_VISIBLE", "EXPLODE_CAR", "BURST_CAR_TYRE", "SET_VEHICLE_DIRT_LEVEL",
  "SET_CAR_ON_GROUND_PROPERLY", "GET_CAR_SPEED", "SET_CAR_FORWARD_SPEED", "SET_CAR_COORDINATES",
  "SET_CAR_HEADING", "GET_CAR_HEADING",
  "IS_PLAYER_PRESSING_HORN", "IS_BUTTON_PRESSED", "GET_POSITION_OF_ANALOGUE_STICKS",
  "SET_TIME_OF_DAY", "FORCE_TIME_OF_DAY", "RELEASE_TIME_OF_DAY", "FORCE_WEATHER_NOW", "RELEASE_WEATHER",
  "SET_CAR_DENSITY_MULTIPLIER", "SET_PED_DENSITY_MULTIPLIER", "SET_WANTED_MULTIPLIER",
  "DISPLAY_HUD", "DISPLAY_RADAR",
  "GIVE_WEAPON_TO_CHAR", "REMOVE_ALL_CHAR_WEAPONS",
  "GET_FIRST_BLIP_INFO_ID", "DOES_BLIP_EXIST", "GET_BLIP_COORDS", "GET_GROUND_Z_FOR_3D_COORD",
  "CREATE_CAM", "DESTROY_CAM", "DOES_CAM_EXIST", "SET_CAM_ACTIVE", "SET_CAM_PROPAGATE",
  "ACTIVATE_SCRIPTED_CAMS", "SET_CAM_POS", "SET_CAM_ROT", "SET_CAM_FOV", "GET_CAM_POS", "GET_CAM_ROT",
  "GET_GAME_CAM", "SET_PLAYER_CONTROL",
  "SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY", "RETUNE_RADIO_UP", "RETUNE_RADIO_DOWN",
  "SKIP_RADIO_FORWARD",
  "APPLY_FORCE_TO_CAR", "GET_NTH_CLOSEST_CAR_NODE_WITH_HEADING", "CREATE_RANDOM_CHAR_AS_DRIVER",
  "CREATE_CHAR_INSIDE_CAR", "TASK_CAR_DRIVE_WANDER", "MARK_CHAR_AS_NO_LONGER_NEEDED", "CAM_IS_SPHERE_VISIBLE",
  "IS_CHAR_SHOOTING", "GET_CURRENT_CHAR_WEAPON", "ADD_EXPLOSION", "GET_CLOSEST_CHAR", "GET_CLOSEST_CAR",
  "GET_CAR_COORDINATES",
};

// Guest handler per command, recorded as the game registers it.
std::array<std::atomic<uint32_t>, size_t(N::COUNT)> handlers{};
std::atomic<uint32_t> wait_thunk{0};
std::mutex registration_mutex;

// Diagnostics: each step from registration to a frame of trainer work.
std::atomic<uint32_t> diag_registrations{0};   // every command the game registered
std::atomic<uint32_t> diag_unreadable_names{0};
std::atomic<bool> diag_wait_failed{false};     // WAIT seen but could not be wrapped
std::atomic<uint64_t> diag_wrapper_calls{0};   // WAIT calls that reached the wrapper
std::atomic<uint64_t> diag_frames{0};          // frames the trainer ran
std::atomic<int> diag_playing{-1};             // last IS_PLAYER_PLAYING result
std::atomic<uint64_t> diag_failed_calls{0};
std::atomic<int> diag_last_failed{-1};         // N index of the last command that could not run

// One argument cell: a plain value, or a pointer to output slot `slot` of
// the scratch frame (pre-filled with `value`).
struct Arg {
  uint32_t value = 0;
  int slot = -1;
  Arg(uint32_t v) : value(v) {}
  Arg(int32_t v) : value(uint32_t(v)) {}
  Arg(float v) : value(F(v)) {}
  Arg(bool v) : value(v ? 1u : 0u) {}
  static Arg Out(int slot, uint32_t initial = 0) {
    Arg a(initial);
    a.slot = slot;
    return a;
  }
};
constexpr int kOutSlots = 8;

// Calls commands from one script-thread WAIT. Each call gets a fresh scratch
// frame below the thread's stack pointer; the VM's own arguments, return
// storage and registers are never touched.
class Script {
 public:
  Script(PPCContext& parent, uint8_t* base) : parent_(parent), base_(base) {}

  bool Call(N name, std::initializer_list<Arg> args = {}) {
    ok_ = false;
    ret_ = 0;
    out_.fill(0);
    const uint32_t handler = handlers[size_t(name)].load(std::memory_order_acquire);
    auto* runtime = rex::Runtime::instance();
    auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
    auto* function = dispatcher && handler ? dispatcher->GetFunction(handler) : nullptr;
    const uint32_t frame = (parent_.r1.u32 - 0x400u) & ~15u;
    if (!function || args.size() > 16 || parent_.r1.u32 < 0x1000 || !Span(base_, frame, 0x400, true)) {
      diag_failed_calls.fetch_add(1, std::memory_order_relaxed);
      diag_last_failed.store(int(name), std::memory_order_relaxed);
      return false;
    }
    const uint32_t context = frame + 0x80, result = frame + 0xC0, arguments = frame + 0xD0,
                   outputs = frame + 0x140;
    Write(base_, frame, parent_.r1.u32);
    Write(base_, context, result);
    Write(base_, context + 4, uint32_t(args.size()));
    Write(base_, context + 8, arguments);
    Write(base_, context + 12, 0);
    for (uint32_t i = 0; i < 4; ++i) Write(base_, result + i * 4, 0);
    for (uint32_t i = 0; i < kOutSlots * 4; ++i) Write(base_, outputs + i * 4, 0);
    uint32_t i = 0;
    for (const Arg& arg : args) {
      if (arg.slot >= 0 && arg.slot < kOutSlots) {
        const uint32_t slot = outputs + uint32_t(arg.slot) * 16;
        Write(base_, slot, arg.value);
        Write(base_, arguments + i * 4, slot);
      } else {
        Write(base_, arguments + i * 4, arg.value);
      }
      ++i;
    }
    PPCContext call = parent_;
    call.r1.u32 = frame;
    call.r3.u32 = context;
    call.lr = 0x82844BA0;
    function(call, base_);
    ret_ = Read(base_, result);
    for (int s = 0; s < kOutSlots; ++s)
      for (int w = 0; w < 4; ++w) out_[size_t(s * 4 + w)] = Read(base_, outputs + uint32_t(s * 16 + w * 4));
    ok_ = true;
    return true;
  }
  // Return value of the last call (0 when it could not run).
  uint32_t Ret() const { return ret_; }
  bool True() const { return ok_ && ret_ != 0; }
  uint32_t Out(int slot, int word = 0) const { return out_[size_t(slot * 4 + word)]; }
  float OutF(int slot, int word = 0) const { return AsFloat(Out(slot, word)); }

 private:
  PPCContext& parent_;
  uint8_t* base_;
  bool ok_ = false;
  uint32_t ret_ = 0;
  std::array<uint32_t, kOutSlots * 4> out_{};
};

// ---------------------------------------------------------------------------
// Settings shared with the UI
// ---------------------------------------------------------------------------

std::array<std::atomic<int>, THEFT4_TRAINER_SETTING_COUNT> settings{};
std::array<std::atomic<bool>, THEFT4_TRAINER_ACTION_COUNT> pending{};
std::mutex spawn_mutex;
std::string spawn_request;  // guarded by spawn_mutex
std::mutex status_mutex;
std::string status_message;  // guarded by status_mutex
std::atomic<uint32_t> status_counter{0};
std::atomic<uint64_t> last_tick_ms{0};

void SetStatus(std::string message) {
  {
    std::lock_guard lock(status_mutex);
    status_message = std::move(message);
  }
  status_counter.fetch_add(1, std::memory_order_release);
}

int Setting(theft4_trainer_setting s) { return settings[s].load(std::memory_order_relaxed); }
bool On(theft4_trainer_setting s) { return Setting(s) != 0; }
bool Take(theft4_trainer_action a) { return pending[a].exchange(false, std::memory_order_acq_rel); }

struct Defaults {
  Defaults() {
    settings[THEFT4_TRAINER_NOCLIP_SPEED] = 3;
    settings[THEFT4_TRAINER_FREECAM_FOV] = 55;
    settings[THEFT4_TRAINER_SPAWN_INSIDE] = 1;
    settings[THEFT4_TRAINER_HOUR] = 12;
    settings[THEFT4_TRAINER_WEATHER] = 1;
    settings[THEFT4_TRAINER_TRAFFIC_DENSITY] = 100;
    settings[THEFT4_TRAINER_PED_DENSITY] = 100;
    settings[THEFT4_TRAINER_POLICE_RESPONSE] = 1;
    settings[THEFT4_TRAINER_BOOST_STRENGTH] = 5;
    settings[THEFT4_TRAINER_EXTRA_TRAFFIC_RATE] = 4;
  }
} defaults;

// ---------------------------------------------------------------------------
// State owned by the script-thread tick
// ---------------------------------------------------------------------------

constexpr uint32_t kButtonBumperLeft = 4, kButtonTriggerLeft = 5, kButtonBumperRight = 6,
                   kButtonTriggerRight = 7;
constexpr float kPi = 3.14159265f;
// GTA IV's local waypoint blip sprite.
constexpr uint32_t kWaypointBlip = 8;

struct Applied {
  bool god = false, never_wanted = false, sprint = false, no_ragdoll = false, invisible = false,
       noclip = false, radio = false, hide_hud = false, freeze_time = false, lock_weather = false;
  int wanted = 0, hour = -1, weather = -1, traffic = 100, peds = 100, police = 1;
  uint32_t car = 0;  // vehicle the car mods were last applied to
  bool car_god = false, car_invisible = false;
  bool brake_held = false;
  uint64_t last_repair = 0, last_weather = 0, last_sprint = 0;
};

struct Tick {
  uint64_t now = 0;
  float dt = 0;
  uint32_t player = 0, ped = 0, car = 0;
  bool in_car = false, dead = false;
  float x = 0, y = 0, z = 0, heading = 0;
};

struct State {
  bool inside = false;
  uint64_t last_run = 0;
  Applied applied;
  // Free camera
  uint32_t cam = 0;
  float cam_x = 0, cam_y = 0, cam_z = 0, cam_pitch = 0, cam_heading = 0;
  // Spawn request in progress
  std::string spawn_model;
  uint32_t spawn_hash = 0;
  uint64_t spawn_deadline = 0;
  // Teleport to waypoint in progress
  bool teleporting = false;
  float tp_x = 0, tp_y = 0, tp_from_x = 0, tp_from_y = 0, tp_from_z = 0;
  uint64_t tp_deadline = 0;
  // Saved position
  bool saved = false;
  float save_x = 0, save_y = 0, save_z = 0, save_heading = 0;
} state;  // only touched from inside the WAIT wrapper, which never nests

float Wrap360(float degrees) {
  degrees = std::fmod(degrees, 360.0f);
  return degrees < 0 ? degrees + 360.0f : degrees;
}
// GTA headings: 0 is north (+Y), increasing counter-clockwise.
void Forward(float heading, float pitch, float& fx, float& fy, float& fz) {
  const float h = heading * kPi / 180.0f, p = pitch * kPi / 180.0f;
  fx = -std::sin(h) * std::cos(p);
  fy = std::cos(h) * std::cos(p);
  fz = std::sin(p);
}
float Stick(uint32_t raw) {
  const float v = float(int32_t(raw)) / 128.0f;
  return std::fabs(v) < 0.12f ? 0.0f : std::clamp(v, -1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------

void ApplyPlayer(Script& s, const Tick& t) {
  Applied& a = state.applied;
#ifdef THEFT4_HAS_GTA4_NATIVE_BACKEND
  // The gameplay hooks keep protection on while God Mode is set; switching
  // it off must also clear the flags those hooks set.
  const bool god = On(THEFT4_TRAINER_GOD_MODE);
  if (a.god && !god) {
    s.Call(N::SET_PLAYER_INVINCIBLE, {t.player, false});
    s.Call(N::SET_CHAR_PROOFS, {t.ped, false, false, false, false, false});
    s.Call(N::SET_CHAR_SUFFERS_CRITICAL_HITS, {t.ped, true});
  }
  a.god = god;
#endif

  const bool never = On(THEFT4_TRAINER_NEVER_WANTED);
  if (never) {
    s.Call(N::SET_MAX_WANTED_LEVEL, {0u});
    s.Call(N::CLEAR_WANTED_LEVEL, {t.player});
  } else if (a.never_wanted) {
    s.Call(N::SET_MAX_WANTED_LEVEL, {6u});
  }
  a.never_wanted = never;

  const int wanted = std::clamp(Setting(THEFT4_TRAINER_WANTED_LEVEL), 0, 6);
  if (wanted != a.wanted && !never) {
    if (wanted == 0) {
      s.Call(N::CLEAR_WANTED_LEVEL, {t.player});
    } else {
      s.Call(N::ALTER_WANTED_LEVEL, {t.player, uint32_t(wanted)});
      s.Call(N::APPLY_WANTED_LEVEL_CHANGE_NOW, {t.player});
    }
  }
  a.wanted = wanted;

  const bool sprint = On(THEFT4_TRAINER_INFINITE_SPRINT);
  if (sprint != a.sprint || (sprint && t.now - a.last_sprint > 1000)) {
    s.Call(N::SET_PLAYER_NEVER_GETS_TIRED, {t.player, sprint});
    a.last_sprint = t.now;
  }
  a.sprint = sprint;

  const bool no_ragdoll = On(THEFT4_TRAINER_NO_RAGDOLL);
  if (no_ragdoll || a.no_ragdoll) {
    s.Call(N::UNLOCK_RAGDOLL, {t.ped, !no_ragdoll});
    s.Call(N::SET_CHAR_CAN_BE_KNOCKED_OFF_BIKE, {t.ped, !no_ragdoll});
  }
  a.no_ragdoll = no_ragdoll;

  const bool invisible = On(THEFT4_TRAINER_INVISIBLE);
  if (invisible || a.invisible) s.Call(N::SET_CHAR_VISIBLE, {t.ped, !invisible});
  a.invisible = invisible;

  const bool radio = On(THEFT4_TRAINER_RADIO_ON_FOOT);
  if (radio != a.radio) s.Call(N::SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY, {radio});
  a.radio = radio;

  if (Take(THEFT4_TRAINER_REFILL_HEALTH_ARMOR)) {
    s.Call(N::SET_CHAR_HEALTH, {t.ped, 200u});
    s.Call(N::GET_CHAR_ARMOUR, {t.ped, Arg::Out(0)});
    const uint32_t armour = s.Out(0);
    if (armour < 100) s.Call(N::ADD_ARMOUR_TO_CHAR, {t.ped, 100u - armour});
    SetStatus("Health and armor refilled");
  }
  if (Take(THEFT4_TRAINER_MINIMUM_HEALTH_ARMOR)) {
    // 101 is the lowest health the game treats as alive.
    s.Call(N::SET_CHAR_HEALTH, {t.ped, 101u});
    s.Call(N::GET_CHAR_ARMOUR, {t.ped, Arg::Out(0)});
    if (const uint32_t armour = s.Out(0)) s.Call(N::ADD_ARMOUR_TO_CHAR, {t.ped, uint32_t(-int32_t(armour))});
    SetStatus("Health and armor at minimum");
  }
  if (Take(THEFT4_TRAINER_RAGDOLL_PLAYER)) {
    if (t.in_car) {
      SetStatus("Leave the vehicle to ragdoll");
    } else {
      s.Call(N::UNLOCK_RAGDOLL, {t.ped, true});
      s.Call(N::SWITCH_PED_TO_RAGDOLL, {t.ped, 1000, 3000, false, false, false, false});
      // No Ragdoll relocks on the next frame, after the ragdoll has started.
      SetStatus("Ragdoll");
    }
  }
  if (Take(THEFT4_TRAINER_RADIO_NEXT)) s.Call(N::RETUNE_RADIO_UP);
  if (Take(THEFT4_TRAINER_RADIO_PREVIOUS)) s.Call(N::RETUNE_RADIO_DOWN);
  if (Take(THEFT4_TRAINER_RADIO_SKIP_TRACK)) s.Call(N::SKIP_RADIO_FORWARD);

  int32_t money_delta = 0;
  bool money_zero = false;
  if (Take(THEFT4_TRAINER_MONEY_ADD_1K)) money_delta += 1000;
  if (Take(THEFT4_TRAINER_MONEY_ADD_10K)) money_delta += 10000;
  if (Take(THEFT4_TRAINER_MONEY_ADD_100K)) money_delta += 100000;
  if (Take(THEFT4_TRAINER_MONEY_ZERO)) money_zero = true;
  if (money_delta || money_zero) {
    s.Call(N::STORE_SCORE, {t.player, Arg::Out(0)});
    const int32_t money = int32_t(s.Out(0));
    const int32_t change = money_zero ? -money : money_delta;
    if (change) s.Call(N::ADD_SCORE, {t.player, change});
    SetStatus("Money: $" + std::to_string(money + change));
  }

  if (Take(THEFT4_TRAINER_GIVE_ALL_WEAPONS)) {
    // TU8 weapon types: bat, knife, grenade, molotov, pistol, Desert Eagle,
    // pump and combat shotguns, Micro SMG, SMG, AK-47, M4, combat and
    // M40A1 sniper rifles, rocket launcher.
    for (uint32_t weapon : {1u, 3u, 4u, 5u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u})
      s.Call(N::GIVE_WEAPON_TO_CHAR, {t.ped, weapon, 9999u, false});
    SetStatus("All weapons given");
  }
  if (Take(THEFT4_TRAINER_REMOVE_ALL_WEAPONS)) {
    s.Call(N::REMOVE_ALL_CHAR_WEAPONS, {t.ped});
    SetStatus("Weapons removed");
  }
}

// Each frame the car the player is in gets the car mods; the previous car
// gets them undone when the player leaves it or a mod is switched off.
void ApplyVehicle(Script& s, const Tick& t) {
  Applied& a = state.applied;
  const bool car_god = On(THEFT4_TRAINER_CAR_GOD_MODE);
  const bool car_invisible = On(THEFT4_TRAINER_CAR_INVISIBLE);
  if (a.car && (a.car != t.car || (a.car_god && !car_god) || (a.car_invisible && !car_invisible))) {
    s.Call(N::DOES_VEHICLE_EXIST, {a.car});
    if (s.True()) {
      if (a.car_god && (a.car != t.car || !car_god)) {
        s.Call(N::SET_CAR_CAN_BE_DAMAGED, {a.car, true});
        s.Call(N::SET_CAR_CAN_BE_VISIBLY_DAMAGED, {a.car, true});
        s.Call(N::SET_CAR_PROOFS, {a.car, false, false, false, false, false});
      }
      if (a.car_invisible && (a.car != t.car || !car_invisible)) s.Call(N::SET_CAR_VISIBLE, {a.car, true});
    }
    if (a.car != t.car) {
      a.car_god = a.car_invisible = false;
      a.car = 0;
    }
  }
  if (!t.car) return;
  a.car = t.car;
  if (car_god) {
    s.Call(N::SET_CAR_CAN_BE_DAMAGED, {t.car, false});
    s.Call(N::SET_CAR_CAN_BE_VISIBLY_DAMAGED, {t.car, false});
    s.Call(N::SET_CAR_PROOFS, {t.car, true, true, true, true, true});
  }
  a.car_god = car_god;
  if (car_invisible) s.Call(N::SET_CAR_VISIBLE, {t.car, false});
  a.car_invisible = car_invisible;

  if (On(THEFT4_TRAINER_AUTO_REPAIR) && t.now - a.last_repair > 500) {
    a.last_repair = t.now;
    s.Call(N::GET_CAR_HEALTH, {t.car, Arg::Out(0)});
    if (int32_t(s.Out(0)) < 1000) s.Call(N::FIX_CAR, {t.car});
  }
  if (On(THEFT4_TRAINER_HORN_BOOST)) {
    s.Call(N::IS_PLAYER_PRESSING_HORN, {t.player});
    if (s.True()) {
      s.Call(N::GET_CAR_SPEED, {t.car, Arg::Out(0)});
      s.Call(N::SET_CAR_FORWARD_SPEED, {t.car, std::min(s.OutF(0) + 1.5f, 70.0f)});
    }
  }
  // Instant Brake stops the car the moment the brake is first pressed while
  // moving, so holding it afterwards still reverses normally.
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonTriggerLeft});
  const bool brake = s.True();
  if (On(THEFT4_TRAINER_INSTANT_BRAKE) && brake && !a.brake_held) {
    s.Call(N::GET_CAR_SPEED, {t.car, Arg::Out(0)});
    if (s.OutF(0) > 2.0f) s.Call(N::SET_CAR_FORWARD_SPEED, {t.car, 0.0f});
  }
  a.brake_held = brake;
}

void VehicleActions(Script& s, const Tick& t) {
  const bool fix = Take(THEFT4_TRAINER_FIX_VEHICLE), explode = Take(THEFT4_TRAINER_EXPLODE_VEHICLE),
             burst = Take(THEFT4_TRAINER_BURST_TYRES), wash = Take(THEFT4_TRAINER_WASH_VEHICLE),
             ground = Take(THEFT4_TRAINER_VEHICLE_ON_GROUND), remove = Take(THEFT4_TRAINER_DELETE_VEHICLE),
             forward = Take(THEFT4_TRAINER_BOOST_FORWARD), up = Take(THEFT4_TRAINER_BOOST_UP);
  if (!(fix || explode || burst || wash || ground || remove || forward || up)) return;
  if (!t.car) {
    SetStatus("Get in a vehicle first");
    return;
  }
  if (fix) { s.Call(N::FIX_CAR, {t.car}); SetStatus("Vehicle fixed"); }
  if (wash) { s.Call(N::SET_VEHICLE_DIRT_LEVEL, {t.car, 0.0f}); SetStatus("Vehicle washed"); }
  if (ground) { s.Call(N::SET_CAR_ON_GROUND_PROPERLY, {t.car}); SetStatus("Vehicle set on its wheels"); }
  if (burst) {
    for (uint32_t tyre = 0; tyre < 6; ++tyre) s.Call(N::BURST_CAR_TYRE, {t.car, tyre});
    SetStatus("Tyres burst");
  }
  const float strength = float(std::clamp(Setting(THEFT4_TRAINER_BOOST_STRENGTH), 1, 10));
  if (forward) {
    // Forward speed is in metres per second along the vehicle's heading.
    s.Call(N::GET_CAR_SPEED, {t.car, Arg::Out(0)});
    s.Call(N::SET_CAR_FORWARD_SPEED, {t.car, std::min(s.OutF(0) + strength * 5.0f, 90.0f)});
    SetStatus("Boost forward");
  }
  if (up) {
    // An upward push in the vehicle's own frame (force type 3, relative).
    s.Call(N::APPLY_FORCE_TO_CAR, {t.car, 3u, 0.0f, 0.0f, strength * 2.0f, 0.0f, 0.0f, 0.0f,
                                   0u, 1u, 1u, 1u});
    SetStatus("Boost up");
  }
  if (explode) { s.Call(N::EXPLODE_CAR, {t.car, true, false}); SetStatus("Vehicle exploded"); }
  if (remove) {
    // Step out beside the car first so the player is not deleted with it.
    float fx, fy, fz;
    Forward(t.heading + 90.0f, 0, fx, fy, fz);
    s.Call(N::WARP_CHAR_FROM_CAR_TO_COORD, {t.ped, t.x + fx * 3.0f, t.y + fy * 3.0f, t.z + 0.5f});
    s.Call(N::DELETE_CAR, {Arg::Out(0, t.car)});
    SetStatus("Vehicle deleted");
  }
}

void Spawn(Script& s, const Tick& t) {
  {
    std::lock_guard lock(spawn_mutex);
    if (!spawn_request.empty()) {
      // A newer choice replaces one still loading.
      if (state.spawn_hash) s.Call(N::MARK_MODEL_AS_NO_LONGER_NEEDED, {state.spawn_hash});
      state.spawn_model = std::move(spawn_request);
      spawn_request.clear();
      state.spawn_hash = ModelHash(state.spawn_model);
      state.spawn_deadline = t.now + 6000;
      s.Call(N::IS_MODEL_IN_CDIMAGE, {state.spawn_hash});
      if (!s.True()) {
        SetStatus(state.spawn_model + " is not in this game or episode");
        state.spawn_hash = 0;
        return;
      }
      s.Call(N::REQUEST_MODEL, {state.spawn_hash});
      SetStatus("Loading " + state.spawn_model + "…");
    }
  }
  if (!state.spawn_hash) return;
  s.Call(N::HAS_MODEL_LOADED, {state.spawn_hash});
  if (!s.True()) {
    if (t.now > state.spawn_deadline) {
      SetStatus(state.spawn_model + " did not load");
      s.Call(N::MARK_MODEL_AS_NO_LONGER_NEEDED, {state.spawn_hash});
      state.spawn_hash = 0;
    } else {
      s.Call(N::REQUEST_MODEL, {state.spawn_hash});
    }
    return;
  }
  float fx, fy, fz;
  Forward(t.heading, 0, fx, fy, fz);
  // Clear of the player, and of the front of the car they may be driving.
  const float ahead = t.car ? 8.0f : 5.0f;
  s.Call(N::CREATE_CAR, {state.spawn_hash, t.x + fx * ahead, t.y + fy * ahead, t.z + 0.5f, Arg::Out(0), true});
  const uint32_t car = s.Out(0);
  s.Call(N::MARK_MODEL_AS_NO_LONGER_NEEDED, {state.spawn_hash});
  state.spawn_hash = 0;
  if (!car) {
    SetStatus(state.spawn_model + " could not be created here");
    return;
  }
  s.Call(N::SET_CAR_HEADING, {car, t.heading});
  s.Call(N::SET_CAR_ON_GROUND_PROPERLY, {car});
  if (On(THEFT4_TRAINER_SPAWN_INSIDE) && !On(THEFT4_TRAINER_NOCLIP))
    s.Call(N::WARP_CHAR_INTO_CAR, {t.ped, car});
  // Hand the car to the world so it is cleaned up like any other traffic.
  s.Call(N::MARK_CAR_AS_NO_LONGER_NEEDED, {Arg::Out(1, car)});
  SetStatus("Spawned " + state.spawn_model);
}

void MoveTo(Script& s, const Tick& t, float x, float y, float z) {
  if (t.car) s.Call(N::SET_CAR_COORDINATES, {t.car, x, y, z});
  else s.Call(N::SET_CHAR_COORDINATES, {t.ped, x, y, z});
}

void Teleport(Script& s, const Tick& t) {
  if (Take(THEFT4_TRAINER_SAVE_POSITION)) {
    state.saved = true;
    state.save_x = t.x; state.save_y = t.y; state.save_z = t.z; state.save_heading = t.heading;
    SetStatus("Position saved");
  }
  if (Take(THEFT4_TRAINER_LOAD_POSITION)) {
    if (!state.saved) {
      SetStatus("No saved position yet");
    } else {
      MoveTo(s, t, state.save_x, state.save_y, state.save_z);
      if (t.car) s.Call(N::SET_CAR_HEADING, {t.car, state.save_heading});
      else s.Call(N::SET_CHAR_HEADING, {t.ped, state.save_heading});
      SetStatus("Returned to saved position");
    }
  }
  if (Take(THEFT4_TRAINER_TELEPORT_WAYPOINT)) {
    s.Call(N::GET_FIRST_BLIP_INFO_ID, {kWaypointBlip});
    const uint32_t blip = s.Ret();
    s.Call(N::DOES_BLIP_EXIST, {blip});
    if (!blip || !s.True()) {
      SetStatus("Set a waypoint on the map first");
    } else {
      s.Call(N::GET_BLIP_COORDS, {blip, Arg::Out(0)});
      state.tp_x = s.OutF(0, 0);
      state.tp_y = s.OutF(0, 1);
      state.tp_from_x = t.x; state.tp_from_y = t.y; state.tp_from_z = t.z;
      state.teleporting = true;
      state.tp_deadline = t.now + 8000;
      // High up while the destination streams in; the ground is found below.
      MoveTo(s, t, state.tp_x, state.tp_y, 400.0f);
      if (!t.car) s.Call(N::FREEZE_CHAR_POSITION, {t.ped, true});
      SetStatus("Teleporting…");
    }
  }
  if (!state.teleporting) return;
  s.Call(N::GET_GROUND_Z_FOR_3D_COORD, {state.tp_x, state.tp_y, 1000.0f, Arg::Out(0)});
  // The query reports 0 until the destination's collision has streamed in.
  const float ground = s.OutF(0);
  const bool found = ground != 0.0f && ground > -100.0f && ground < 1000.0f;
  if (found || t.now > state.tp_deadline) {
    // Never drop the player from the holding height: without ground, go back.
    if (found) MoveTo(s, t, state.tp_x, state.tp_y, ground + 1.0f);
    else MoveTo(s, t, state.tp_from_x, state.tp_from_y, state.tp_from_z);
    if (!t.car && !On(THEFT4_TRAINER_NOCLIP)) s.Call(N::FREEZE_CHAR_POSITION, {t.ped, false});
    state.teleporting = false;
    SetStatus(found ? "Teleported to waypoint" : "Waypoint area did not load; returned to start");
  } else {
    MoveTo(s, t, state.tp_x, state.tp_y, 400.0f);
  }
}

// Left stick moves, LB/RB go down/up, RT goes faster. Without the free
// camera the game camera steers; with it the player follows the camera.
void Noclip(Script& s, const Tick& t) {
  Applied& a = state.applied;
  const bool want = On(THEFT4_TRAINER_NOCLIP) && !t.in_car && !t.dead;
  if (!want) {
    if (a.noclip) {
      s.Call(N::SET_CHAR_COLLISION, {t.ped, true});
      s.Call(N::FREEZE_CHAR_POSITION, {t.ped, false});
      a.noclip = false;
    }
    if (On(THEFT4_TRAINER_NOCLIP) && t.in_car) {
      settings[THEFT4_TRAINER_NOCLIP] = 0;
      SetStatus("No Clip works on foot");
    }
    return;
  }
  if (!a.noclip) {
    s.Call(N::SET_CHAR_COLLISION, {t.ped, false});
    s.Call(N::FREEZE_CHAR_POSITION, {t.ped, true});
    a.noclip = true;
  }
  if (state.cam) {
    s.Call(N::SET_CHAR_COORDINATES_NO_OFFSET, {t.ped, state.cam_x, state.cam_y, state.cam_z - 1.0f});
    s.Call(N::SET_CHAR_HEADING, {t.ped, state.cam_heading});
    return;
  }
  s.Call(N::GET_POSITION_OF_ANALOGUE_STICKS, {0u, Arg::Out(0), Arg::Out(1), Arg::Out(2), Arg::Out(3)});
  const float lx = Stick(s.Out(0)), ly = Stick(s.Out(1));
  s.Call(N::GET_GAME_CAM, {Arg::Out(0)});
  const uint32_t game_cam = s.Out(0);
  s.Call(N::GET_CAM_ROT, {game_cam, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
  const float pitch = s.OutF(0), heading = Wrap360(s.OutF(2));
  float fx, fy, fz;
  Forward(heading, pitch, fx, fy, fz);
  const float rx = std::cos(heading * kPi / 180.0f), ry = std::sin(heading * kPi / 180.0f);
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonBumperLeft});
  const float down = s.True() ? 1.0f : 0.0f;
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonBumperRight});
  const float up = s.True() ? 1.0f : 0.0f;
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonTriggerRight});
  const float boost = s.True() ? 4.0f : 1.0f;
  const float speed = float(std::clamp(Setting(THEFT4_TRAINER_NOCLIP_SPEED), 1, 10)) * 4.0f * boost * t.dt;
  const float forward = -ly;  // stick up is negative
  const float x = t.x + (fx * forward + rx * lx) * speed;
  const float y = t.y + (fy * forward + ry * lx) * speed;
  const float z = t.z + (fz * forward + (up - down)) * speed;
  s.Call(N::SET_CHAR_COORDINATES_NO_OFFSET, {t.ped, x, y, z});
  s.Call(N::SET_CHAR_HEADING, {t.ped, heading});
}

// A scripted camera flown with the sticks. The player keeps still unless No
// Clip is on, in which case the player follows the camera.
void FreeCamera(Script& s, const Tick& t) {
  const bool want = On(THEFT4_TRAINER_FREECAM) && !t.dead;
  if (!want) {
    if (state.cam) {
      s.Call(N::ACTIVATE_SCRIPTED_CAMS, {false, false});
      s.Call(N::DOES_CAM_EXIST, {state.cam});
      if (s.True()) s.Call(N::DESTROY_CAM, {state.cam});
      s.Call(N::SET_PLAYER_CONTROL, {t.player, true});
      state.cam = 0;
    }
    return;
  }
  if (state.cam) {
    s.Call(N::DOES_CAM_EXIST, {state.cam});
    if (!s.True()) state.cam = 0;  // the owning script ended; make another
  }
  if (!state.cam) {
    s.Call(N::GET_GAME_CAM, {Arg::Out(0)});
    const uint32_t game_cam = s.Out(0);
    s.Call(N::GET_CAM_POS, {game_cam, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
    state.cam_x = s.OutF(0); state.cam_y = s.OutF(1); state.cam_z = s.OutF(2);
    s.Call(N::GET_CAM_ROT, {game_cam, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
    state.cam_pitch = s.OutF(0); state.cam_heading = Wrap360(s.OutF(2));
    s.Call(N::CREATE_CAM, {14u, Arg::Out(0)});
    state.cam = s.Out(0);
    if (!state.cam) {
      settings[THEFT4_TRAINER_FREECAM] = 0;
      SetStatus("Free camera is unavailable right now");
      return;
    }
    s.Call(N::SET_CAM_POS, {state.cam, state.cam_x, state.cam_y, state.cam_z});
    s.Call(N::SET_CAM_ROT, {state.cam, state.cam_pitch, 0.0f, state.cam_heading});
    s.Call(N::SET_CAM_ACTIVE, {state.cam, true});
    s.Call(N::SET_CAM_PROPAGATE, {state.cam, true});
    s.Call(N::ACTIVATE_SCRIPTED_CAMS, {true, true});
    s.Call(N::SET_PLAYER_CONTROL, {t.player, false});
  }
  s.Call(N::GET_POSITION_OF_ANALOGUE_STICKS, {0u, Arg::Out(0), Arg::Out(1), Arg::Out(2), Arg::Out(3)});
  const float lx = Stick(s.Out(0)), ly = Stick(s.Out(1)), rx = Stick(s.Out(2)), ry = Stick(s.Out(3));
  state.cam_heading = Wrap360(state.cam_heading - rx * 120.0f * t.dt);
  state.cam_pitch = std::clamp(state.cam_pitch - ry * 90.0f * t.dt, -89.0f, 89.0f);
  float fx, fy, fz;
  Forward(state.cam_heading, state.cam_pitch, fx, fy, fz);
  const float sx = std::cos(state.cam_heading * kPi / 180.0f), sy = std::sin(state.cam_heading * kPi / 180.0f);
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonBumperLeft});
  const float down = s.True() ? 1.0f : 0.0f;
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonBumperRight});
  const float up = s.True() ? 1.0f : 0.0f;
  s.Call(N::IS_BUTTON_PRESSED, {0u, kButtonTriggerRight});
  const float boost = s.True() ? 4.0f : 1.0f;
  const float speed = float(std::clamp(Setting(THEFT4_TRAINER_NOCLIP_SPEED), 1, 10)) * 4.0f * boost * t.dt;
  state.cam_x += (fx * -ly + sx * lx) * speed;
  state.cam_y += (fy * -ly + sy * lx) * speed;
  state.cam_z += (fz * -ly + (up - down)) * speed;
  s.Call(N::SET_CAM_POS, {state.cam, state.cam_x, state.cam_y, state.cam_z});
  s.Call(N::SET_CAM_ROT, {state.cam, state.cam_pitch, 0.0f, state.cam_heading});
  s.Call(N::SET_CAM_FOV, {state.cam, float(std::clamp(Setting(THEFT4_TRAINER_FREECAM_FOV), 30, 100))});
}

void ApplyWorld(Script& s, const Tick& t) {
  Applied& a = state.applied;
  const int hour = std::clamp(Setting(THEFT4_TRAINER_HOUR), 0, 23);
  const bool freeze = On(THEFT4_TRAINER_FREEZE_TIME);
  if (hour != a.hour && a.hour >= 0) {
    if (freeze) s.Call(N::FORCE_TIME_OF_DAY, {uint32_t(hour), 0u});
    else s.Call(N::SET_TIME_OF_DAY, {uint32_t(hour), 0u});
  }
  if (freeze != a.freeze_time) {
    if (freeze) s.Call(N::FORCE_TIME_OF_DAY, {uint32_t(hour), 0u});
    else s.Call(N::RELEASE_TIME_OF_DAY);
  }
  a.hour = hour;
  a.freeze_time = freeze;

  const int weather = std::clamp(Setting(THEFT4_TRAINER_WEATHER), 0, 7);
  const bool lock = On(THEFT4_TRAINER_LOCK_WEATHER);
  if ((weather != a.weather && a.weather >= 0) || (lock && !a.lock_weather) ||
      (lock && t.now - a.last_weather > 10000)) {
    s.Call(N::FORCE_WEATHER_NOW, {uint32_t(weather)});
    a.last_weather = t.now;
  }
  if (!lock && a.lock_weather) s.Call(N::RELEASE_WEATHER);
  a.weather = weather;
  a.lock_weather = lock;

  // Densities and police response are only written while changed from the
  // game's default, so missions that set their own keep control otherwise.
  const int traffic = std::clamp(Setting(THEFT4_TRAINER_TRAFFIC_DENSITY), 0, 200);
  if (traffic != 100 || a.traffic != 100) s.Call(N::SET_CAR_DENSITY_MULTIPLIER, {float(traffic) / 100.0f});
  a.traffic = traffic;
  const int peds = std::clamp(Setting(THEFT4_TRAINER_PED_DENSITY), 0, 200);
  if (peds != 100 || a.peds != 100) s.Call(N::SET_PED_DENSITY_MULTIPLIER, {float(peds) / 100.0f});
  a.peds = peds;
  constexpr std::array<float, 4> kPolice = {0.25f, 1.0f, 2.0f, 4.0f};
  const int police = std::clamp(Setting(THEFT4_TRAINER_POLICE_RESPONSE), 0, 3);
  if (police != 1 || a.police != 1) s.Call(N::SET_WANTED_MULTIPLIER, {kPolice[size_t(police)]});
  a.police = police;

  const bool hide = On(THEFT4_TRAINER_HIDE_HUD);
  if (hide || a.hide_hud) {
    s.Call(N::DISPLAY_HUD, {!hide});
    s.Call(N::DISPLAY_RADAR, {!hide});
  }
  a.hide_hud = hide;
}

// Extra traffic: every few seconds one vehicle from the chosen group joins
// the streets on a road node out of view, with a driver who wanders, and is
// then handed to the world like any ambient vehicle. Models that are not in
// the running game or episode are skipped.
struct Injector {
  uint64_t next = 0, deadline = 0;
  uint32_t model = 0, driver = 0;
  bool police = false;
} injector;

constexpr std::array<std::string_view, 12> kSportsTraffic = {
  "banshee", "comet", "coquette", "feltzer", "infernus", "sultanrs", "supergt", "turismo",
  "bullet", "f620", "superd", "buffalo"};
constexpr std::array<std::string_view, 7> kPoliceTraffic = {
  "police", "police2", "polpatriot", "noose", "police3", "police4", "fbi"};
constexpr std::array<std::string_view, 20> kUnusualTraffic = {
  "airtug", "ripley", "trash", "mrtasty", "bus", "flatbed", "biff", "mule", "yankee", "benson",
  "packer", "phantom", "forklift", "steed", "boxville", "caddy", "avan", "firetruk", "ambulance",
  "stockade"};
// GTA IV's police ped type.
constexpr uint32_t kPedTypeCop = 6;

uint32_t Random(uint32_t limit) {
  static uint32_t seed = uint32_t(NowMs()) | 1u;
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return limit ? seed % limit : 0;
}

void ReleaseInjection(Script& s) {
  if (injector.model) s.Call(N::MARK_MODEL_AS_NO_LONGER_NEEDED, {injector.model});
  if (injector.driver) s.Call(N::MARK_MODEL_AS_NO_LONGER_NEEDED, {injector.driver});
  injector.model = injector.driver = 0;
}

void ExtraTraffic(Script& s, const Tick& t) {
  const int mode = std::clamp(Setting(THEFT4_TRAINER_EXTRA_TRAFFIC), 0, 3);
  const uint64_t gap = uint64_t(std::clamp(Setting(THEFT4_TRAINER_EXTRA_TRAFFIC_RATE), 2, 15)) * 1000;
  if (!mode) {
    ReleaseInjection(s);
    return;
  }
  if (!injector.model) {
    if (t.now < injector.next) return;
    std::string_view name;
    if (mode == 1) name = kSportsTraffic[Random(kSportsTraffic.size())];
    else if (mode == 2) name = kPoliceTraffic[Random(kPoliceTraffic.size())];
    else name = kUnusualTraffic[Random(kUnusualTraffic.size())];
    const uint32_t model = ModelHash(name);
    s.Call(N::IS_MODEL_IN_CDIMAGE, {model});
    if (!s.True()) {
      injector.next = t.now + 250;  // not in this game or episode; pick again shortly
      return;
    }
    injector.model = model;
    injector.police = mode == 2;
    injector.driver = 0;
    if (injector.police) {
      injector.driver = ModelHash(name == "noose" ? "m_y_swat" : "m_y_cop");
      s.Call(N::REQUEST_MODEL, {injector.driver});
    }
    s.Call(N::REQUEST_MODEL, {model});
    injector.deadline = t.now + 6000;
    return;
  }
  s.Call(N::HAS_MODEL_LOADED, {injector.model});
  bool loaded = s.True();
  if (loaded && injector.driver) {
    s.Call(N::HAS_MODEL_LOADED, {injector.driver});
    loaded = s.True();
  }
  if (!loaded) {
    if (t.now > injector.deadline) {
      ReleaseInjection(s);
      injector.next = t.now + gap;
    }
    return;
  }
  // A road node 90-140 m away, in a random direction, that the camera
  // cannot see, so vehicles never appear in plain sight.
  const float angle = float(Random(360)) * kPi / 180.0f;
  const float distance = 90.0f + float(Random(50));
  s.Call(N::GET_NTH_CLOSEST_CAR_NODE_WITH_HEADING,
         {t.x + std::cos(angle) * distance, t.y + std::sin(angle) * distance, t.z, 1u + Random(3),
          Arg::Out(0), Arg::Out(1), Arg::Out(2), Arg::Out(3)});
  const float x = s.OutF(0), y = s.OutF(1), z = s.OutF(2), heading = s.OutF(3);
  const float dx = x - t.x, dy = y - t.y;
  if ((x == 0.0f && y == 0.0f) || dx * dx + dy * dy < 60.0f * 60.0f) {
    if (t.now > injector.deadline) { ReleaseInjection(s); injector.next = t.now + gap; }
    return;  // try another node next frame
  }
  s.Call(N::GET_GAME_CAM, {Arg::Out(0)});
  s.Call(N::CAM_IS_SPHERE_VISIBLE, {s.Out(0), x, y, z, 4.0f});
  if (s.True()) {
    if (t.now > injector.deadline) { ReleaseInjection(s); injector.next = t.now + gap; }
    return;
  }
  s.Call(N::CREATE_CAR, {injector.model, x, y, z + 0.5f, Arg::Out(0), true});
  const uint32_t car = s.Out(0);
  if (car) {
    s.Call(N::SET_CAR_HEADING, {car, heading});
    s.Call(N::SET_CAR_ON_GROUND_PROPERLY, {car});
    if (injector.police) s.Call(N::CREATE_CHAR_INSIDE_CAR, {car, kPedTypeCop, injector.driver, Arg::Out(1)});
    else s.Call(N::CREATE_RANDOM_CHAR_AS_DRIVER, {car, Arg::Out(1)});
    const uint32_t driver = s.Out(1);
    if (driver) {
      s.Call(N::TASK_CAR_DRIVE_WANDER, {driver, car, injector.police ? 14.0f : 11.0f, 0u});
      s.Call(N::MARK_CHAR_AS_NO_LONGER_NEEDED, {Arg::Out(2, driver)});
    }
    s.Call(N::MARK_CAR_AS_NO_LONGER_NEEDED, {Arg::Out(3, car)});
  }
  ReleaseInjection(s);
  injector.next = t.now + gap;
}

// Explosive Rounds. No script command reports where a bullet lands, so each
// shot follows the gameplay camera's aim line, the line bullets travel
// along: the explosion goes on the first ped or vehicle near the line, or
// where it meets the ground. A wall is not seen, so a shot into one explodes
// on the ground beyond it. Works for whichever character is playing.
uint64_t next_round_ms = 0;  // only touched from inside the WAIT wrapper

void ExplosiveRounds(Script& s, const Tick& t) {
  if (!On(THEFT4_TRAINER_EXPLOSIVE_ROUNDS) || t.now < next_round_ms) return;
  s.Call(N::IS_CHAR_SHOOTING, {t.ped});
  if (!s.True()) return;
  // Firearms only: unarmed, melee, thrown weapons and the rocket launcher
  // (TU8 types 0-6 and 18) keep their own behaviour.
  s.Call(N::GET_CURRENT_CHAR_WEAPON, {t.ped, Arg::Out(0)});
  const uint32_t weapon = s.Out(0);
  if (weapon <= 6 || weapon == 18) return;
  // One explosion per shot: automatic weapons fire about ten rounds a second.
  next_round_ms = t.now + 180;

  s.Call(N::GET_GAME_CAM, {Arg::Out(0)});
  const uint32_t cam = s.Out(0);
  s.Call(N::GET_CAM_POS, {cam, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
  const float ox = s.OutF(0), oy = s.OutF(1), oz = s.OutF(2);
  s.Call(N::GET_CAM_ROT, {cam, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
  float fx, fy, fz;
  Forward(Wrap360(s.OutF(2)), s.OutF(0), fx, fy, fz);

  constexpr float kStep = 1.5f, kRange = 120.0f;
  bool hit = false;
  float hx = 0, hy = 0, hz = 0;
  for (float d = 2.0f; d <= kRange && !hit; d += kStep) {
    const float px = ox + fx * d, py = oy + fy * d, pz = oz + fz * d;
    s.Call(N::GET_CLOSEST_CHAR, {px, py, pz, 1.2f, true, true, Arg::Out(0)});
    const uint32_t ped = s.True() ? s.Out(0) : 0;
    if (ped && ped != t.ped) {
      s.Call(N::GET_CHAR_COORDINATES, {ped, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
      hx = s.OutF(0); hy = s.OutF(1); hz = s.OutF(2);
      hit = true;
      break;
    }
    s.Call(N::GET_CLOSEST_CAR, {px, py, pz, 2.5f, false, 70u});
    const uint32_t car = s.Ret();
    if (car && car != t.car) {
      s.Call(N::GET_CAR_COORDINATES, {car, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
      hx = s.OutF(0); hy = s.OutF(1); hz = s.OutF(2);
      hit = true;
      break;
    }
    // The highest surface below this point; 0 means its collision has not
    // streamed in.
    s.Call(N::GET_GROUND_Z_FOR_3D_COORD, {px, py, pz, Arg::Out(0)});
    const float ground = s.OutF(0);
    if (ground != 0.0f && pz - ground < 0.4f) {
      hx = px; hy = py; hz = ground;
      hit = true;
    }
  }
  if (!hit) return;
  const float dx = hx - t.x, dy = hy - t.y, dz = hz - t.z;
  if (dx * dx + dy * dy + dz * dz < 3.0f * 3.0f) return;  // never at the shooter's feet
  // Type 0 is the grenade explosion.
  s.Call(N::ADD_EXPLOSION, {hx, hy, hz, 0u, 3.0f, true, false, 0.4f});
}

void Run(PPCContext& ctx, uint8_t* base) {
  const uint64_t now = NowMs();
  // Every running script calls WAIT; run once per game frame at most.
  if (now - state.last_run < 14) return;
  Script s(ctx, base);
  Tick t;
  t.now = now;
  t.dt = state.last_run ? std::min(float(now - state.last_run) / 1000.0f, 0.1f) : 0.016f;
  state.last_run = now;
  last_tick_ms.store(now, std::memory_order_release);
  diag_frames.fetch_add(1, std::memory_order_relaxed);

  if (!s.Call(N::GET_PLAYER_ID)) return;
  t.player = s.Ret();
  s.Call(N::IS_PLAYER_PLAYING, {t.player});
  diag_playing.store(s.True() ? 1 : 0, std::memory_order_relaxed);
  if (!s.True()) return;
  s.Call(N::GET_PLAYER_CHAR, {t.player, Arg::Out(0)});
  t.ped = s.Out(0);
  if (!t.ped) return;
  s.Call(N::IS_CHAR_DEAD, {t.ped});
  t.dead = s.True();
  s.Call(N::IS_CHAR_IN_ANY_CAR, {t.ped});
  t.in_car = s.True();
  if (t.in_car) {
    s.Call(N::STORE_CAR_CHAR_IS_IN_NO_SAVE, {t.ped, Arg::Out(0)});
    t.car = s.Out(0);
  }
  s.Call(N::GET_CHAR_COORDINATES, {t.ped, Arg::Out(0), Arg::Out(1), Arg::Out(2)});
  t.x = s.OutF(0); t.y = s.OutF(1); t.z = s.OutF(2);
  s.Call(N::GET_CHAR_HEADING, {t.ped, Arg::Out(0)});
  t.heading = s.OutF(0);

  ApplyWorld(s, t);
  if (t.dead) return;
  ExtraTraffic(s, t);
  ApplyPlayer(s, t);
  ExplosiveRounds(s, t);
  ApplyVehicle(s, t);
  VehicleActions(s, t);
  Spawn(s, t);
  FreeCamera(s, t);
  Noclip(s, t);
  Teleport(s, t);
}

void WaitWrapper(PPCContext& ctx, uint8_t* base) {
  diag_wrapper_calls.fetch_add(1, std::memory_order_relaxed);
  if (!state.inside) {
    state.inside = true;
    Run(ctx, base);
    state.inside = false;
  }
  const uint32_t original = handlers[size_t(N::WAIT)].load(std::memory_order_acquire);
  auto* runtime = rex::Runtime::instance();
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (auto* wait = dispatcher && original ? dispatcher->GetFunction(original) : nullptr) wait(ctx, base);
}

}  // namespace

extern "C" {

void theft4_trainer_set(theft4_trainer_setting setting, int value) {
  if (int(setting) < 0 || int(setting) >= THEFT4_TRAINER_SETTING_COUNT) return;
  settings[setting].store(value, std::memory_order_relaxed);
#ifdef THEFT4_HAS_GTA4_NATIVE_BACKEND
  if (setting == THEFT4_TRAINER_GOD_MODE || setting == THEFT4_TRAINER_UNLIMITED_AMMO)
    gta4::mods::ConfigureGameplay(On(THEFT4_TRAINER_GOD_MODE), On(THEFT4_TRAINER_UNLIMITED_AMMO));
#endif
}

int theft4_trainer_get(theft4_trainer_setting setting) {
  if (int(setting) < 0 || int(setting) >= THEFT4_TRAINER_SETTING_COUNT) return 0;
  return settings[setting].load(std::memory_order_relaxed);
}

void theft4_trainer_run(theft4_trainer_action action) {
  if (int(action) < 0 || int(action) >= THEFT4_TRAINER_ACTION_COUNT) return;
  pending[action].store(true, std::memory_order_release);
}

void theft4_trainer_spawn_vehicle(const char* model) {
  if (!model || !*model) return;
  std::lock_guard lock(spawn_mutex);
  spawn_request = model;
}

uint32_t theft4_trainer_status(char* message, size_t capacity) {
  if (message && capacity) {
    std::lock_guard lock(status_mutex);
    const size_t n = std::min(capacity - 1, status_message.size());
    std::memcpy(message, status_message.data(), n);
    message[n] = 0;
  }
  return status_counter.load(std::memory_order_acquire);
}

void theft4_trainer_diagnostics(char* text, size_t capacity) {
  if (!text || !capacity) return;
  size_t found = 0;
  for (const auto& handler : handlers) found += handler.load(std::memory_order_relaxed) ? 1 : 0;
  const uint32_t registered = diag_registrations.load(std::memory_order_relaxed);
  const char* wait = wait_thunk.load(std::memory_order_relaxed) ? "WAIT wrapped"
                   : diag_wait_failed.load(std::memory_order_relaxed) ? "WAIT wrap FAILED"
                   : handlers[size_t(N::WAIT)].load(std::memory_order_relaxed) ? "WAIT not wrapped"
                   : "WAIT not seen";
  const int playing = diag_playing.load(std::memory_order_relaxed);
  const int failed = diag_last_failed.load(std::memory_order_relaxed);
  std::snprintf(text, capacity,
                "Cmds %u (%zu/%zu used, %u unreadable) · %s · WAIT calls %llu · frames %llu · "
                "playing %s · failed calls %llu%s%s",
                registered, found, kNames.size(), diag_unreadable_names.load(std::memory_order_relaxed),
                wait, (unsigned long long)diag_wrapper_calls.load(std::memory_order_relaxed),
                (unsigned long long)diag_frames.load(std::memory_order_relaxed),
                playing < 0 ? "?" : playing ? "yes" : "no",
                (unsigned long long)diag_failed_calls.load(std::memory_order_relaxed),
                failed >= 0 ? " last " : "", failed >= 0 ? kNames[size_t(failed)].data() : "");
}

int theft4_trainer_active(void) {
  const uint64_t last = last_tick_ms.load(std::memory_order_acquire);
  return wait_thunk.load(std::memory_order_acquire) && last && NowMs() - last < 2000 ? 1 : 0;
}

}  // extern "C"

// Script command registration: record the handlers the trainer uses.
extern "C" void sub_82845600(PPCContext& ctx, uint8_t* base) {
  diag_registrations.fetch_add(1, std::memory_order_relaxed);
  const std::string name = GuestString(base, ctx.r3.u32);
  if (name.empty()) diag_unreadable_names.fetch_add(1, std::memory_order_relaxed);
  if (!name.empty()) {
    for (size_t i = 1; i < kNames.size(); ++i) {
      if (kNames[i] != name) continue;
      handlers[i].store(ctx.r4.u32, std::memory_order_release);
      break;
    }
  }
  __imp__sub_82845600(ctx, base);
}

// Command table insert (table, name hash, handler). sub_82845600 hashes the
// name and calls this, but WAIT and the other core commands are registered
// by sub_82846068, which calls it directly, so WAIT is wrapped here. The
// hash is GET_HASH_KEY's (sub_82846F98). sub_82846AE8 re-inserts existing
// entries when the table grows; an entry that already holds the thunk is
// left alone.
extern "C" void sub_82846948(PPCContext& ctx, uint8_t* base) {
  static const uint32_t kWaitHash = ModelHash("WAIT");
  constexpr uint32_t kCommandTable = 0x831927A0;
  if (ctx.r3.u32 == kCommandTable && ctx.r4.u32 == kWaitHash && ctx.r5.u32 &&
      ctx.r5.u32 != wait_thunk.load(std::memory_order_acquire)) {
    std::lock_guard lock(registration_mutex);
    handlers[size_t(N::WAIT)].store(ctx.r5.u32, std::memory_order_release);
    auto* runtime = rex::Runtime::instance();
    auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
    uint32_t thunk = wait_thunk.load(std::memory_order_acquire);
    if (!thunk && dispatcher) {
      thunk = dispatcher->AllocateThunk(WaitWrapper, uint32_t(ctx.lr));
      wait_thunk.store(thunk, std::memory_order_release);
    }
    if (thunk) {
      ctx.r5.u32 = thunk;
      REXLOG_INFO("Theft4 trainer: WAIT {:08X} wrapped by thunk {:08X}",
                  handlers[size_t(N::WAIT)].load(), thunk);
    } else {
      diag_wait_failed.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("Theft4 trainer: could not wrap WAIT; trainer inactive");
    }
  }
  __imp__sub_82846948(ctx, base);
}
