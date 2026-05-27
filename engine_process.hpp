#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

class EngineProcess {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    EngineProcess(const std::string& engine_path, bool verbose_output);
    EngineProcess(const std::vector<std::string>& command, bool verbose_output);
    ~EngineProcess();

    EngineProcess(const EngineProcess&) = delete;
    EngineProcess& operator=(const EngineProcess&) = delete;
    EngineProcess(EngineProcess&&) = delete;
    EngineProcess& operator=(EngineProcess&&) = delete;

    void send(const std::string& cmd);
    bool waitReadable(int timeout_ms);
    std::optional<std::string> readLine(bool print_diagnostics = true);
    bool hasExited();
    std::optional<std::string> abnormalTermination();
    std::string terminationMessage(const std::string& context);
};

void waitForToken(EngineProcess& engine, const std::string& token);
void initializeEngine(EngineProcess& engine,
                      const std::vector<std::string>& setoptions,
                      int threads);
void initializeReference(EngineProcess& engine);
void resetReference(EngineProcess& engine);
