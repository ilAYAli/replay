#pragma once

#include <future>
#include <memory>
#include <string>

struct MoveValidation {
    bool ok = false;
    std::string error;
    int quality = 0;
    std::string label;
    std::string best_move;  // Stockfish's preferred move
    int cp_loss = 0;        // Centipawn loss (how much worse the move is)
};

std::string formatQualityPercent(int quality, bool color);
std::string formatQualityPercent(double quality, bool color);
std::string formatValidation(const MoveValidation& validation, bool color);

class ValidatorWorker {
public:
    explicit ValidatorWorker(const std::string& validator_path);
    ~ValidatorWorker();

    ValidatorWorker(const ValidatorWorker&) = delete;
    ValidatorWorker& operator=(const ValidatorWorker&) = delete;

    std::future<MoveValidation> submit(const std::string& position, const std::string& played_move, int depth);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
