#pragma once

#include <filesystem>
#include <string>

#include "gta4_installer.h"

namespace theft4 {

// Episodes are copied by the player beside the game, as Documents/dlc/TBOGT
// and Documents/dlc/TLAD. The runtime lists every folder under its marketplace
// root as installed DLC, so that folder is used only when each episode in it
// is complete; otherwise the private, empty startup root keeps the base game
// running and the summary says what is wrong.
struct MarketplaceRoot {
  std::filesystem::path path;
  std::string summary;
};

inline std::filesystem::path EpisodeFolder(const std::filesystem::path& game) {
  auto normal = game.lexically_normal();
  if (normal.filename().empty()) normal = normal.parent_path();
  return normal.parent_path() / "dlc";
}

inline MarketplaceRoot ResolveMarketplaceRoot(const std::filesystem::path& game,
                                              const std::filesystem::path& support) {
  using gta4::install::Episode;
  const auto dlc = EpisodeFolder(game);
  MarketplaceRoot result{support / "marketplace", {}};
  bool any_ready = false;
  bool any_incomplete = false;
  for (const auto episode : {Episode::kTbogt, Episode::kTlad}) {
    const char* name = gta4::install::EpisodeDirectory(episode);
    std::error_code error;
    if (!std::filesystem::is_directory(dlc / name, error)) continue;
    const bool ready = gta4::install::IsEpisodeReady(dlc, episode);
    any_ready |= ready;
    any_incomplete |= !ready;
    if (!result.summary.empty()) result.summary += ", ";
    result.summary += std::string(name) + (ready ? " installed" : " incomplete");
  }
  if (result.summary.empty()) result.summary = "no episodes installed";
  if (any_ready && !any_incomplete) result.path = dlc;
  else if (any_incomplete) result.summary += " (episodes disabled until fixed)";
  return result;
}

}  // namespace theft4
