#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace theft4::mods {
// Build/reuse a private RPF containing the bundled table. Empty means this
// install has only loose files. Never writes to the user's game directory.
std::filesystem::path PrepareTimeCycleArchive(const std::filesystem::path& game,
    const std::filesystem::path& bundle, const std::filesystem::path& cache);

// Build/reuse a private copy of an RPF2 archive in which entry_path (for
// example "data/handling.dat") holds replacement. The copy is named
// <prefix>-<hash>.rpf under cache; the source archive is never modified.
std::filesystem::path PrepareArchiveReplacement(const std::filesystem::path& source,
    const std::filesystem::path& key_path, std::string_view entry_path,
    const std::vector<uint8_t>& replacement, const std::filesystem::path& cache,
    const std::string& prefix, const std::string& label);

// The bytes of entry_path inside an RPF2 archive, decompressed if needed.
std::vector<uint8_t> ReadArchiveEntry(const std::filesystem::path& source,
    const std::filesystem::path& key_path, std::string_view entry_path, const std::string& label);
}
