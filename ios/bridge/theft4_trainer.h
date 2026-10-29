#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// In-game trainer. Settings are plain values the UI may change at any time
// from the main thread; the game applies them from inside a running script
// thread (the game's WAIT command), once per game frame.

typedef enum {
    // Player
    THEFT4_TRAINER_GOD_MODE,
    THEFT4_TRAINER_UNLIMITED_AMMO,
    THEFT4_TRAINER_NEVER_WANTED,
    THEFT4_TRAINER_WANTED_LEVEL,      // 0-6, applied when changed
    THEFT4_TRAINER_INFINITE_SPRINT,
    THEFT4_TRAINER_NO_RAGDOLL,
    THEFT4_TRAINER_INVISIBLE,
    THEFT4_TRAINER_NOCLIP,
    THEFT4_TRAINER_NOCLIP_SPEED,      // 1-10
    THEFT4_TRAINER_FREECAM,
    THEFT4_TRAINER_FREECAM_FOV,       // 30-100 degrees
    THEFT4_TRAINER_RADIO_ON_FOOT,
    // Vehicle
    THEFT4_TRAINER_CAR_GOD_MODE,
    THEFT4_TRAINER_AUTO_REPAIR,
    THEFT4_TRAINER_CAR_INVISIBLE,
    THEFT4_TRAINER_HORN_BOOST,
    THEFT4_TRAINER_INSTANT_BRAKE,
    THEFT4_TRAINER_SPAWN_INSIDE,
    // World
    THEFT4_TRAINER_HOUR,              // 0-23, applied when changed
    THEFT4_TRAINER_FREEZE_TIME,
    THEFT4_TRAINER_WEATHER,           // 0-7, applied when changed
    THEFT4_TRAINER_LOCK_WEATHER,
    THEFT4_TRAINER_TRAFFIC_DENSITY,   // percent, 0-200 in steps of 25
    THEFT4_TRAINER_PED_DENSITY,       // percent, 0-200 in steps of 25
    THEFT4_TRAINER_POLICE_RESPONSE,   // 0 relaxed, 1 normal, 2 aggressive, 3 relentless
    THEFT4_TRAINER_HIDE_HUD,
    THEFT4_TRAINER_BOOST_STRENGTH,    // 1-10
    THEFT4_TRAINER_EXTRA_TRAFFIC,     // 0 off, 1 sports cars, 2 police, 3 unusual vehicles
    THEFT4_TRAINER_EXTRA_TRAFFIC_RATE,// seconds between added vehicles, 2-15
    THEFT4_TRAINER_EXPLOSIVE_ROUNDS,  // the player's firearm shots explode where they land
    THEFT4_TRAINER_SETTING_COUNT
} theft4_trainer_setting;

typedef enum {
    THEFT4_TRAINER_REFILL_HEALTH_ARMOR,
    THEFT4_TRAINER_MINIMUM_HEALTH_ARMOR,
    THEFT4_TRAINER_RAGDOLL_PLAYER,
    THEFT4_TRAINER_RADIO_NEXT,
    THEFT4_TRAINER_RADIO_PREVIOUS,
    THEFT4_TRAINER_RADIO_SKIP_TRACK,
    THEFT4_TRAINER_MONEY_ADD_1K,
    THEFT4_TRAINER_MONEY_ADD_10K,
    THEFT4_TRAINER_MONEY_ADD_100K,
    THEFT4_TRAINER_MONEY_ZERO,
    THEFT4_TRAINER_FIX_VEHICLE,
    THEFT4_TRAINER_EXPLODE_VEHICLE,
    THEFT4_TRAINER_BURST_TYRES,
    THEFT4_TRAINER_WASH_VEHICLE,
    THEFT4_TRAINER_VEHICLE_ON_GROUND,
    THEFT4_TRAINER_DELETE_VEHICLE,
    THEFT4_TRAINER_TELEPORT_WAYPOINT,
    THEFT4_TRAINER_SAVE_POSITION,
    THEFT4_TRAINER_LOAD_POSITION,
    THEFT4_TRAINER_GIVE_ALL_WEAPONS,
    THEFT4_TRAINER_REMOVE_ALL_WEAPONS,
    THEFT4_TRAINER_BOOST_FORWARD,
    THEFT4_TRAINER_BOOST_UP,
    THEFT4_TRAINER_ACTION_COUNT
} theft4_trainer_action;

void theft4_trainer_set(theft4_trainer_setting setting, int value);
int theft4_trainer_get(theft4_trainer_setting setting);
void theft4_trainer_run(theft4_trainer_action action);
// Model name as the game knows it, for example "infernus".
void theft4_trainer_spawn_vehicle(const char* model);
// Copies the newest result message; returns a counter that changes with it.
uint32_t theft4_trainer_status(char* message, size_t capacity);
// 1 once the game has registered its script commands and the trainer has
// run inside a script thread within the last two seconds.
int theft4_trainer_active(void);
// One short line each, for the mod menu's Diagnostics page and log exports:
// how far the trainer got (commands registered, WAIT wrapped, frames run)
// and what the launcher's episode choice saw at the game's start screen.
void theft4_trainer_diagnostics(char* text, size_t capacity);
void theft4_episode_diagnostics(char* text, size_t capacity);

#ifdef __cplusplus
}
#endif
