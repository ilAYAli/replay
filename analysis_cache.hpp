#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "reference_limit.hpp"

constexpr long long kDefaultMaxReplayNodes = 100'000'000;

struct EngineConfig {
    std::string hash;
    std::string nnue2_hash;
};

struct AnalysisCache {
    std::string key;
    std::string provenance;
};

EngineConfig probeEngineConfig(const std::string& engine_path,
                               const std::vector<std::string>& setoptions);

std::filesystem::path analysisPath(const std::filesystem::path& logfile,
                                   const std::string& analysis_key,
                                   const std::string& analysis_target);

AnalysisCache buildAnalysisCache(const std::filesystem::path& logfile,
                                 const EngineConfig& candidate,
                                 const EngineConfig& reference,
                                 bool time_mode,
                                 long long max_replay_nodes,
                                 const ReferenceLimit& reference_limit,
                                 const std::string& analysis_target);
