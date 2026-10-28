#include "theft4_handling_mod.h"
#include "theft4_timecycle_archive.h"

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/string.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace theft4::mods {
namespace {

constexpr const char* kLabel = "Vehicle handling";

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += line + "\r\n";
    return out;
}

std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream in(line);
    std::string token;
    while (in >> token) tokens.push_back(token);
    return tokens;
}

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool IsMarker(char c) { return c == '!' || c == '$' || c == '%' || c == '^'; }

// A data line's key: "NAME" for a vehicle's main line, "!NAME", "$NAME",
// "%NAME" or "^NAME" for its bike, boat, helicopter or plane line. Empty for
// comments ("; ..." and the commented-out "#NAME" lines) and blank lines.
std::string Key(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return {};
    const std::string& first = tokens[0];
    if (first[0] == ';' || first[0] == '#') return {};
    if (IsMarker(first[0])) {
        if (first.size() > 1) return std::string(1, first[0]) + Upper(first.substr(1));
        return tokens.size() > 1 ? std::string(1, first[0]) + Upper(tokens[1]) : std::string{};
    }
    return Upper(first);
}

std::string Join(const std::vector<std::string>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) out += (i ? "\t" : "") + tokens[i];
    return out;
}

std::set<std::string> IdList(const std::string& ids) {
    std::set<std::string> set;
    for (const auto& line : SplitLines(ids))
        if (!line.empty()) set.insert(Upper(line));
    return set;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string(kLabel) + ": cannot read " + path.filename().string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) throw std::runtime_error(std::string(kLabel) + ": cannot write " + path.filename().string());
    }
    std::filesystem::rename(temporary, path);
}

std::string Format(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    std::string s = buffer;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.push_back('0');
    return s;
}

// Scales or offsets one numeric column; non-numeric text is left alone.
void Adjust(std::vector<std::string>& fields, size_t index, double scale, double offset = 0) {
    if (index >= fields.size()) return;
    char* end = nullptr;
    const double value = std::strtod(fields[index].c_str(), &end);
    if (!end || *end || end == fields[index].c_str()) return;
    fields[index] = Format(value * scale + offset);
}

}  // namespace

std::string MergeHandling(const std::string& original, const std::string& mod,
                          const std::string& extra_ids) {
    std::map<std::string, std::vector<std::string>> mod_lines;
    for (const auto& line : SplitLines(mod)) {
        const auto tokens = Tokens(line);
        const auto key = Key(tokens);
        if (!key.empty()) mod_lines[key] = tokens;
    }
    const auto extra = IdList(extra_ids);
    std::set<std::string> seen;
    std::vector<std::string> out;
    size_t last_main = SIZE_MAX;
    for (const auto& line : SplitLines(original)) {
        const auto tokens = Tokens(line);
        const auto key = Key(tokens);
        if (!key.empty()) seen.insert(key);
        auto found = key.empty() ? mod_lines.end() : mod_lines.find(key);
        if (found == mod_lines.end()) {
            out.push_back(line);
        } else {
            // Keep the game's own column count: missing trailing columns
            // come from the original, extra ones (PC-only) are dropped.
            auto fields = found->second;
            if (fields.size() < tokens.size())
                fields.insert(fields.end(), tokens.begin() + long(fields.size()), tokens.end());
            fields.resize(tokens.size());
            out.push_back(Join(fields));
        }
        if (!key.empty() && !IsMarker(key[0])) last_main = out.size() - 1;
    }
    // New vehicles' main lines go right after the last original main line.
    std::vector<std::string> added;
    for (const auto& id : extra) {
        auto found = mod_lines.find(id);
        if (found != mod_lines.end() && !seen.count(id)) added.push_back(Join(found->second));
    }
    if (!added.empty()) {
        const size_t at = last_main == SIZE_MAX ? out.size() : last_main + 1;
        out.insert(out.begin() + long(at), added.begin(), added.end());
    }
    return JoinLines(out);
}

std::string JellyHandling(const std::string& handling) {
    // Vehicles with a boat, helicopter or plane line keep their values, as
    // do trains; Jelly is for cars and bikes.
    std::set<std::string> skip = {"CABLECAR", "SUBWAY", "TRAIN"};
    const auto lines = SplitLines(handling);
    for (const auto& line : lines) {
        const auto key = Key(Tokens(line));
        if (!key.empty() && (key[0] == '$' || key[0] == '%' || key[0] == '^')) skip.insert(key.substr(1));
    }
    std::vector<std::string> out;
    for (const auto& line : lines) {
        auto fields = Tokens(line);
        const auto key = Key(fields);
        if (key.empty() || IsMarker(key[0]) || skip.count(key) || fields.size() < 28) {
            out.push_back(line);
            continue;
        }
        Adjust(fields, 1, 0.75);        // B  mass: lighter
        Adjust(fields, 2, 0.6);         // C  drag: carries speed, floaty
        Adjust(fields, 6, 1.0, 0.30);   // G  centre of mass height: top-heavy, rolls over
        Adjust(fields, 21, 0.5);        // Sf suspension force: soft
        Adjust(fields, 22, 0.35);       // Scd compression damping: bouncy
        Adjust(fields, 23, 0.35);       // Srd rebound damping: bouncy
        Adjust(fields, 24, 1.8);        // Su upper travel
        Adjust(fields, 25, 1.8);        // Sl lower travel
        out.push_back(Join(fields));
    }
    return JoinLines(out);
}

std::string PatchVehiclesIde(const std::string& original_ide, const std::string& mod_ide,
                             const std::string& original_handling, const std::string& mod_handling,
                             std::string& added_ids) {
    std::set<std::string> original_ids, mod_ids;
    for (const auto& line : SplitLines(original_handling)) {
        const auto key = Key(Tokens(line));
        if (!key.empty() && !IsMarker(key[0])) original_ids.insert(key);
    }
    for (const auto& line : SplitLines(mod_handling)) {
        const auto key = Key(Tokens(line));
        if (!key.empty() && !IsMarker(key[0])) mod_ids.insert(key);
    }
    auto split = [](const std::string& line) {
        std::vector<std::string> fields;
        std::string field;
        std::istringstream in(line);
        while (std::getline(in, field, ',')) fields.push_back(field);
        return fields;
    };
    auto trim = [](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
    };
    // model -> handling ID in the mod's file (cars section rows: model, txd,
    // type, handling, ...).
    std::map<std::string, std::string> mod_handling_of;
    for (const auto& line : SplitLines(mod_ide)) {
        const auto fields = split(line);
        if (fields.size() > 4) mod_handling_of[Upper(trim(fields[0]))] = Upper(trim(fields[3]));
    }
    std::set<std::string> added;
    std::vector<std::string> out;
    for (const auto& line : SplitLines(original_ide)) {
        auto fields = split(line);
        if (fields.size() > 4) {
            const auto found = mod_handling_of.find(Upper(trim(fields[0])));
            const auto current = Upper(trim(fields[3]));
            if (found != mod_handling_of.end() && found->second != current &&
                !original_ids.count(found->second) && mod_ids.count(found->second)) {
                // Keep the field's leading whitespace so columns stay aligned.
                const auto lead = fields[3].substr(0, fields[3].find_first_not_of(" \t"));
                fields[3] = lead + found->second;
                added.insert(found->second);
                std::string rebuilt;
                for (size_t i = 0; i < fields.size(); ++i) rebuilt += (i ? "," : "") + fields[i];
                out.push_back(rebuilt);
                continue;
            }
        }
        out.push_back(line);
    }
    added_ids.clear();
    for (const auto& id : added) added_ids += id + "\n";
    return JoinLines(out);
}

namespace {

// Serves only the named file from a private directory and passes every
// other name through to the original directory (VFS links are prefix-based).
class SingleFileDevice final : public rex::filesystem::HostPathDevice {
public:
    SingleFileDevice(const std::string& name, const std::filesystem::path& directory,
                     std::string file, rex::filesystem::Entry* original)
        : HostPathDevice(name, directory, true), file_(std::move(file)), original_(original) {}
    rex::filesystem::Entry* ResolvePath(std::string_view path) override {
        const auto name = rex::string::utf8_canonicalize_guest_path(path);
        if (rex::string::utf8_equal_case(name, file_) ||
            rex::string::utf8_equal_case(name, "\\" + file_))
            return HostPathDevice::ResolvePath(file_);
        return original_ ? original_->ResolvePath(path) : nullptr;
    }
private:
    std::string file_;
    rex::filesystem::Entry* original_;
};

std::string ApplyPreset(const std::string& original, const std::string& mod,
                        const std::string& extra, HandlingPreset preset) {
    const auto realistic = MergeHandling(original, mod, extra);
    return preset == HandlingPreset::kJelly ? JellyHandling(realistic) : realistic;
}

std::string PresetName(HandlingPreset preset) {
    return preset == HandlingPreset::kJelly ? "jelly" : "realistic";
}

}  // namespace

PreparedHandling PrepareBaseHandling(const std::filesystem::path& game,
                                     const std::filesystem::path& bundle,
                                     const std::filesystem::path& cache, HandlingPreset preset) {
    const auto loose = game / "common" / "data" / "handling.dat";
    const auto archive = game / "common.rpf";
    const auto key = game / "aes_key.bin";
    std::string original;
    if (std::filesystem::exists(loose)) {
        original = ReadFile(loose);
    } else if (std::filesystem::exists(archive)) {
        const auto bytes = ReadArchiveEntry(archive, key, "data/handling.dat", kLabel);
        original.assign(bytes.begin(), bytes.end());
    } else {
        throw std::runtime_error(std::string(kLabel) + ": the game's handling.dat was not found.");
    }
    const auto text = ApplyPreset(original, ReadFile(bundle / "handling-base.dat"), {}, preset);
    PreparedHandling prepared;
    prepared.loose_file = cache / PresetName(preset) / "handling.dat";
    WriteFile(prepared.loose_file, text);
    if (std::filesystem::exists(archive)) {
        const std::vector<uint8_t> bytes(text.begin(), text.end());
        prepared.archive = PrepareArchiveReplacement(archive, key, "data/handling.dat", bytes, cache,
                                                     "common-" + PresetName(preset), kLabel);
    }
    return prepared;
}

void MountBaseHandling(rex::filesystem::VirtualFileSystem& fs, const PreparedHandling& prepared) {
    // Loose copy of the game's files, when the install has them.
    if (auto* data = fs.ResolvePath("game:\\common\\data"); data && data->GetChild("handling.dat")) {
        auto device = std::make_unique<SingleFileDevice>("\\Device\\Theft4Handling",
            prepared.loose_file.parent_path(), "handling.dat", data);
        if (!device->Initialize() || !device->ResolvePath("handling.dat"))
            throw std::runtime_error(std::string(kLabel) + ": cannot open the prepared handling file.");
        fs.RegisterDevice(std::move(device));
        fs.RegisterSymbolicLink("\\Device\\Harddisk0\\Partition1\\common\\data\\handling.dat",
                                "\\Device\\Theft4Handling\\handling.dat");
    }
    // The archive the game itself reads.
    if (!prepared.archive.empty()) {
        auto* root = fs.ResolvePath("game:");
        if (!root || !root->GetChild("common.rpf"))
            throw std::runtime_error(std::string(kLabel) + ": common.rpf is unavailable.");
        auto device = std::make_unique<SingleFileDevice>("\\Device\\Theft4HandlingArchive",
            prepared.archive.parent_path(), prepared.archive.filename().string(), root);
        if (!device->Initialize() || !device->ResolvePath(prepared.archive.filename().string()))
            throw std::runtime_error(std::string(kLabel) + ": cannot open the prepared archive.");
        fs.RegisterDevice(std::move(device));
        fs.RegisterSymbolicLink("\\Device\\Harddisk0\\Partition1\\common.rpf",
                                "\\Device\\Theft4HandlingArchive\\" + prepared.archive.filename().string());
    }
}

std::filesystem::path PrepareEpisodeHandling(const std::filesystem::path& dlc_root,
                                             const std::filesystem::path& bundle,
                                             const std::filesystem::path& overlay,
                                             HandlingPreset preset) {
    std::error_code error;
    std::filesystem::remove_all(overlay, error);
    std::filesystem::create_directories(overlay);
    const std::map<std::string, std::string> files = {{"TBOGT", "tbogt"}, {"TLAD", "tlad"}};
    for (const auto& [episode, suffix] : files) {
        const auto source = dlc_root / episode;
        if (!std::filesystem::is_directory(source, error)) continue;
        const auto target = overlay / episode;
        const auto data = std::filesystem::path("common") / "data";
        const auto handling = source / data / "handling.dat";
        const auto ide = source / data / "vehicles.ide";
        const auto mod_handling_path = bundle / ("handling-" + suffix + ".dat");
        const auto mod_ide_path = bundle / ("vehicles-" + suffix + ".ide");
        // Without its handling.dat the episode is mirrored unchanged.
        const bool patch = std::filesystem::exists(handling);
        // The runtime lists directories itself and does not follow linked
        // directories, so directories are real and only files are links.
        std::filesystem::create_directories(target);
        for (auto it = std::filesystem::recursive_directory_iterator(source);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            const auto relative = std::filesystem::relative(it->path(), source);
            if (it->is_directory()) {
                std::filesystem::create_directories(target / relative);
                continue;
            }
            const auto name = Upper(relative.generic_string());
            if (patch && (name == "COMMON/DATA/HANDLING.DAT" || name == "COMMON/DATA/VEHICLES.IDE")) continue;
            std::filesystem::create_symlink(std::filesystem::absolute(it->path()), target / relative);
        }
        if (!patch) continue;
        const auto original = ReadFile(handling);
        std::string extra;
        if (std::filesystem::exists(ide)) {
            std::string patched = ReadFile(ide);
            if (std::filesystem::exists(mod_ide_path) && std::filesystem::exists(mod_handling_path))
                patched = PatchVehiclesIde(patched, ReadFile(mod_ide_path), original,
                                           ReadFile(mod_handling_path), extra);
            WriteFile(target / data / "vehicles.ide", patched);
        }
        const auto mod = std::filesystem::exists(mod_handling_path) ? ReadFile(mod_handling_path) : std::string{};
        WriteFile(target / data / "handling.dat", ApplyPreset(original, mod, extra, preset));
    }
    return overlay;
}

}  // namespace theft4::mods
