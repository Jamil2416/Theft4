#pragma once
#include <filesystem>
#include <string>

namespace rex::filesystem { class VirtualFileSystem; }

namespace theft4::mods {

// Handling presets. Realistic uses Realistic Handling & Physics by
// Shturmovik2, merged per vehicle into the game's own handling.dat so every
// vehicle the running game defines keeps a line. Jelly starts from
// Realistic and makes vehicles soft, bouncy, light and top-heavy.
enum class HandlingPreset { kOriginal = 0, kRealistic = 1, kJelly = 2 };

// --- Text transforms (pure; no file system access) ---

// The original file with each vehicle line the mod also defines replaced by
// the mod's line, trimmed or padded to the original line's column count.
// Mod-only vehicles are added only when listed in extra_ids (one per line).
std::string MergeHandling(const std::string& original, const std::string& mod,
                          const std::string& extra_ids = {});
// Jelly values for every car and bike line; boats, aircraft and trains keep theirs.
std::string JellyHandling(const std::string& handling);
// The original vehicles.ide with handling IDs changed where the mod's file
// gives a model its own handling line that mod_handling defines but
// original_handling does not. Returns the IDs it pointed at (one per line)
// through added_ids.
std::string PatchVehiclesIde(const std::string& original_ide, const std::string& mod_ide,
                             const std::string& original_handling, const std::string& mod_handling,
                             std::string& added_ids);

// --- Base game ---

// Prepares the base game's handling for preset: a loose replacement file
// and, when the game has common.rpf, a private copy of that archive with the
// replacement inside. Throws on failure; the caller keeps the original.
struct PreparedHandling {
    std::filesystem::path loose_file;  // cache/.../handling.dat
    std::filesystem::path archive;     // empty for loose-only installs
};
PreparedHandling PrepareBaseHandling(const std::filesystem::path& game,
                                     const std::filesystem::path& bundle,
                                     const std::filesystem::path& cache, HandlingPreset preset);
void MountBaseHandling(rex::filesystem::VirtualFileSystem& fs, const PreparedHandling& prepared);

// --- Episodes ---

// Builds overlay/<EPISODE> for every episode folder in dlc_root: real
// directories, symbolic links to every original file, and real modified
// common/data/handling.dat and vehicles.ide. Returns the overlay to use as
// the marketplace root. Nothing in dlc_root is written. Throws on failure.
std::filesystem::path PrepareEpisodeHandling(const std::filesystem::path& dlc_root,
                                             const std::filesystem::path& bundle,
                                             const std::filesystem::path& overlay,
                                             HandlingPreset preset);

}  // namespace theft4::mods
