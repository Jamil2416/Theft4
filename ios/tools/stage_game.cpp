#include "gta4_installer.h"
#include <rex/logging.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>

namespace {

int Usage() {
    std::fprintf(stderr,
        "Usage: theft4_stage <base ISO> <update package> <NEW install directory> [episodes]\n"
        "       theft4_stage --dlc-only <EXISTING install directory> <episodes>\n"
        "Episodes: --tbogt <package or folder> and/or --tlad <package or folder>\n");
    return 2;
}

// Reads "--tbogt X" / "--tlad X" pairs; returns false on anything else.
bool ParseEpisodes(int argc, char** argv, int first, std::vector<gta4::install::DlcSelection>& out) {
    for (int i = first; i < argc; i += 2) {
        if (i + 1 >= argc) return false;
        if (!std::strcmp(argv[i], "--tbogt"))
            out.push_back({gta4::install::Episode::kTbogt, argv[i + 1]});
        else if (!std::strcmp(argv[i], "--tlad"))
            out.push_back({gta4::install::Episode::kTlad, argv[i + 1]});
        else
            return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const bool dlc_only = argc >= 2 && !std::strcmp(argv[1], "--dlc-only");
    gta4::install::Selection selection;
    if (dlc_only) {
        if (argc < 5 || !ParseEpisodes(argc, argv, 3, selection.dlc_sources)) return Usage();
    } else {
        if (argc < 4 || !ParseEpisodes(argc, argv, 4, selection.dlc_sources)) return Usage();
        selection.game_source = argv[1];
        selection.update_source = argv[2];
    }
    // A command-line tool: show the SDK's own errors (for example a disc
    // image or package that cannot be mounted) beside the final result.
    rex::LogConfig logging;
    logging.log_to_console = true;
    rex::InitLogging(logging);
    int status = 2;
    try {
        const auto destination =
            std::filesystem::absolute(argv[dlc_only ? 2 : 3]).lexically_normal();
        if (dlc_only) {
            // Adds episodes beside an earlier staging; the installer publishes
            // only dlc/<episode> and refuses without a valid game/ tree.
            if (!std::filesystem::is_directory(destination / "game"))
                throw std::runtime_error("Destination must be an earlier theft4_stage output");
        } else if (destination == destination.root_path() ||
                   !std::filesystem::create_directory(destination)) {
            // The upstream installer replaces its staging/backup paths. Only let
            // this developer utility operate inside a newly claimed directory.
            throw std::runtime_error("Destination must be a new directory with an existing parent");
        }
        gta4::install::Progress progress;
        std::jthread reporter([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::printf("Staging: %llu / %llu bytes\n",
                    (unsigned long long)progress.copied_bytes.load(),
                    (unsigned long long)progress.total_bytes.load());
                std::fflush(stdout);
                for (int i = 0; i < 20 && !stop.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        auto result = gta4::install::Install(selection, destination, progress);
        if (result.success)
            result = gta4::install::VerifyInstall(destination / "game", destination / "dlc");
        reporter.request_stop();
        status = result.success ? 0 : 1;
        // path::c_str() is wide on Windows, and path::string() throws there for
        // names outside the ANSI code page; the UTF-8 form works on every host.
        const auto destination_utf8 = destination.u8string();
        const std::string message = result.success
            ? std::string(destination_utf8.begin(), destination_utf8.end())
            : result.error;
        std::printf("Staging %s: %s\n", result.success ? "VERIFIED" : "FAILED", message.c_str());
        if (result.success) {
            for (const auto episode : {gta4::install::Episode::kTbogt, gta4::install::Episode::kTlad}) {
                if (gta4::install::IsEpisodeReady(destination / "dlc", episode))
                    std::printf("Episode ready: dlc/%s (copy to the iPad as Documents/dlc/%s)\n",
                                gta4::install::EpisodeDirectory(episode),
                                gta4::install::EpisodeDirectory(episode));
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Staging failed: %s\n", error.what());
    }
    rex::ShutdownLogging();
    return status;
}
