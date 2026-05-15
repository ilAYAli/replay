#include "analysis_cache.hpp"

#include "engine_process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>

namespace {

struct FileOptionSummary {
    std::string details;
    std::string nnue2_hash = "none";
};

bool startsWith(const std::string& line, const std::string& prefix) {
    return line.rfind(prefix, 0) == 0;
}

std::string trim(std::string text) {
    while (!text.empty() && std::isspace((unsigned char)text.front()))
        text.erase(text.begin());
    while (!text.empty() && std::isspace((unsigned char)text.back()))
        text.pop_back();
    return text;
}

std::string lower(std::string text) {
    for (char& ch : text)
        ch = (char)std::tolower((unsigned char)ch);
    return text;
}

void hashAppend(uint64_t& hash, std::string_view data) {
    constexpr uint64_t prime = 1099511628211ULL;
    for (char ch : data) {
        auto byte = static_cast<unsigned char>(ch);
        hash ^= byte;
        hash *= prime;
    }
}

std::string hashString(std::string_view data, size_t length = 12) {
    uint64_t hash = 1469598103934665603ULL;
    hashAppend(hash, data);
    std::string hex = fmt::format("{:016x}", hash);
    return hex.substr(0, std::min(length, hex.size()));
}

std::string hashFileContent(const std::filesystem::path& path, size_t length = 12) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "missing";

    uint64_t hash = 1469598103934665603ULL;
    char buffer[65536];
    while (file) {
        file.read(buffer, sizeof(buffer));
        hashAppend(hash, std::string_view(buffer, (size_t)file.gcount()));
    }

    std::string hex = fmt::format("{:016x}", hash);
    return hex.substr(0, std::min(length, hex.size()));
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines)
        text += line + "\n";
    return text;
}

std::filesystem::path expandPath(std::string value) {
    value = trim(value);
    if (value.size() >= 2
     && ((value.front() == '"' && value.back() == '"')
      || (value.front() == '\'' && value.back() == '\'')))
        value = value.substr(1, value.size() - 2);

    if (startsWith(value, "~/")) {
        const char* home = std::getenv("HOME");
        if (home)
            value = std::string(home) + value.substr(1);
    }

    std::filesystem::path path = value;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (!ec)
            return canonical;
    }

    return path;
}

bool isFileOptionName(const std::string& name) {
    std::string option = lower(name);
    return option.find("nnue") != std::string::npos
        || option.find("file") != std::string::npos;
}

std::optional<std::pair<std::string, std::string>> parseSetoptionValue(const std::string& line) {
    if (!startsWith(line, "setoption name "))
        return std::nullopt;

    std::string rest = line.substr(15);
    size_t value_pos = rest.find(" value ");
    if (value_pos == std::string::npos)
        return std::pair{trim(rest), std::string{}};

    return std::pair{trim(rest.substr(0, value_pos)), trim(rest.substr(value_pos + 7))};
}

std::optional<std::pair<std::string, std::string>> parseUciDefaultValue(const std::string& line) {
    if (!startsWith(line, "option name "))
        return std::nullopt;

    size_t type_pos = line.find(" type ", 12);
    if (type_pos == std::string::npos)
        return std::nullopt;

    std::string name = trim(line.substr(12, type_pos - 12));
    size_t default_pos = line.find(" default ", type_pos + 6);
    if (default_pos == std::string::npos)
        return std::pair{name, std::string{}};

    return std::pair{name, trim(line.substr(default_pos + 9))};
}

std::optional<std::string> enyoVersionIdentity(const std::string& line) {
    std::string marker = "Enyo Release ";
    size_t start = line.find(marker);
    if (start == std::string::npos)
        return std::nullopt;

    start += marker.size();
    size_t end = line.find(" built ", start);
    std::string version = trim(line.substr(start, end == std::string::npos ? end : end - start));
    if (version.empty())
        return std::nullopt;
    return "id enyo-version " + version;
}

std::string cacheUciText(const std::string& uci_text) {
    std::istringstream input(uci_text);
    std::string output;
    std::string line;
    std::optional<std::string> enyo_version;
    while (std::getline(input, line)) {
        if (startsWith(line, "Using config file:"))
            continue;

        auto version = enyoVersionIdentity(line);
        if (version)
            enyo_version = version;

        auto option = parseUciDefaultValue(line);
        if (option && isFileOptionName(option->first)) {
            output += fmt::format("option name {} file-option\n", option->first);
            continue;
        }

        output += line + "\n";
    }

    if (enyo_version)
        return *enyo_version + "\n";

    return output;
}

void addFileOptionDetail(std::string& details,
                         std::unordered_map<std::string, std::string>& file_hashes,
                         std::string& nnue2_hash,
                         const std::string& name,
                         const std::string& value) {
    if (!isFileOptionName(name) || trim(value).empty())
        return;

    auto path = expandPath(value);
    std::error_code ec;
    std::string option = lower(name);
    details += name;
    if (std::filesystem::is_regular_file(path, ec)) {
        std::string key = path.string();
        if (!file_hashes.contains(key))
            file_hashes[key] = hashFileContent(path, 16);
        details += fmt::format(" hash={}", file_hashes[key]);
        if (option.find("nnue2") != std::string::npos)
            nnue2_hash = file_hashes[key].substr(0, 8);
    } else {
        details += fmt::format(" hash=missing value={}", path.filename().string());
        if (option.find("nnue2") != std::string::npos)
            nnue2_hash = "missing";
    }
    details += "\n";
}

FileOptionSummary fileOptionDetails(const std::string& uci_text,
                                    const std::vector<std::string>& setoptions) {
    std::unordered_map<std::string, std::string> file_hashes;
    FileOptionSummary summary;

    std::istringstream uci_lines(uci_text);
    std::string line;
    while (std::getline(uci_lines, line)) {
        auto option = parseUciDefaultValue(line);
        if (option)
            addFileOptionDetail(summary.details, file_hashes, summary.nnue2_hash,
                                option->first, option->second);
    }

    for (const auto& setoption : setoptions) {
        auto option = parseSetoptionValue(setoption);
        if (option)
            addFileOptionDetail(summary.details, file_hashes, summary.nnue2_hash,
                                option->first, option->second);
    }

    return summary;
}

std::string referenceLimitName(const ReferenceLimit& limit) {
    switch (limit.kind) {
    case ReferenceLimitKind::Nodes:
        return fmt::format("nodes{}", limit.value);
    case ReferenceLimitKind::Depth:
        return fmt::format("depth{}", limit.value);
    case ReferenceLimitKind::LoggedDepth:
        return "log-depth";
    }

    return "unknown";
}

std::string referenceLimitDisplay(const ReferenceLimit& limit) {
    switch (limit.kind) {
    case ReferenceLimitKind::Nodes:
        return fmt::format("ref-nodes {}", limit.value);
    case ReferenceLimitKind::Depth:
        return fmt::format("ref-depth {}", limit.value);
    case ReferenceLimitKind::LoggedDepth:
        return "ref-depth log";
    }

    return "ref-limit unknown";
}

std::string analysisModeName(bool time_mode,
                             const ReferenceLimit& reference_limit,
                             const std::string& analysis_target) {
    std::string mode = analysis_target == "log"
        ? "log"
        : (time_mode ? "time" : "replay_nodes");
    if (reference_limit.kind == ReferenceLimitKind::Nodes && reference_limit.value == kDefaultReferenceNodes)
        return mode;
    return fmt::format("{}_{}", mode, referenceLimitName(reference_limit));
}

} // namespace

EngineConfig probeEngineConfig(const std::string& engine_path,
                               const std::vector<std::string>& setoptions) {
    EngineProcess engine(engine_path, false);
    engine.send("uci");

    std::string uci_text;
    while (true) {
        if (!engine.waitReadable(30000)) {
            if (engine.hasExited())
                throw std::runtime_error("engine exited while waiting for uciok");
            throw std::runtime_error("timed out waiting for uciok");
        }

        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error("engine closed stdout while waiting for uciok");
        uci_text += *line + "\n";
        if (*line == "uciok")
            break;
    }

    std::string setoption_text = joinLines(setoptions);
    auto file_options = fileOptionDetails(uci_text, setoptions);
    std::string identity_text = cacheUciText(uci_text);
    std::string config_hash = hashString(fmt::format(
        "uci={}\nsetoptions={}\nfiles={}\n",
        identity_text,
        setoption_text,
        file_options.details));

    return {
        config_hash,
        file_options.nnue2_hash
    };
}

std::filesystem::path analysisPath(const std::filesystem::path& logfile,
                                   const std::string& analysis_key,
                                   const std::string& analysis_target) {
    if (analysis_target == "log")
        return logfile.parent_path() / fmt::format("{}.analysis", logfile.stem().string());

    return logfile.parent_path() / fmt::format("{}.{}_analysis",
                                               logfile.stem().string(),
                                               analysis_key);
}

AnalysisCache buildAnalysisCache(const std::filesystem::path& logfile,
                                 const EngineConfig& candidate,
                                 const EngineConfig& reference,
                                 bool time_mode,
                                 long long max_replay_nodes,
                                 const ReferenceLimit& reference_limit,
                                 const std::string& analysis_target) {
    auto pgn_path = logfile;
    pgn_path.replace_extension(".pgn");

    std::string log_hash = hashFileContent(logfile);
    std::string pgn_hash = std::filesystem::exists(pgn_path) ? hashFileContent(pgn_path) : "none";
    std::string input_hash = hashString(fmt::format("log={}\npgn={}\n", log_hash, pgn_hash));
    std::string mode = analysisModeName(time_mode, reference_limit, analysis_target);
    std::string mode_text = fmt::format("mode={}\ntime={}\ntarget={}\n",
                                        mode, time_mode, analysis_target);
    if (analysis_target != "log" && !time_mode && max_replay_nodes != kDefaultMaxReplayNodes)
        mode_text += fmt::format("replay-node-cap={}\n", max_replay_nodes);
    mode_text += fmt::format("ref-limit={}\nref-state=fresh\n",
                             referenceLimitName(reference_limit));
    std::string mode_hash = hashString(mode_text);

    std::string key = hashString(fmt::format(
        "replay-cache-v24\ncandidate={}\nreference={}\nlog={}\npgn={}\nmode={}\n",
        candidate.hash, reference.hash, log_hash, pgn_hash, mode_hash));

    std::string provenance = fmt::format(
        "analysis-key {} | candidate cfg {} | reference cfg {} | log {} | target {} | {} | ref-state fresh | nnue2 {}",
        key,
        candidate.hash,
        reference.hash,
        input_hash,
        analysis_target,
        referenceLimitDisplay(reference_limit),
        candidate.nnue2_hash);
    if (analysis_target != "log" && !time_mode) {
        provenance += " | candidate nodes";
        if (max_replay_nodes != kDefaultMaxReplayNodes)
            provenance += fmt::format(" max {}", max_replay_nodes);
    }

    return {key, provenance};
}
