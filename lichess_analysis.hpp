#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

constexpr int kFishnetManualRequestNodes = 1'000'000;
constexpr int kFishnetChunkPositions = 6;
constexpr int kDefaultReferenceNodes = kFishnetManualRequestNodes * kFishnetChunkPositions
                                     / (kFishnetChunkPositions + 1);

enum class ScoreKind {
    None,
    Cp,
    Mate
};

struct Score {
    ScoreKind kind = ScoreKind::None;
    int value = 0;
};

Score scoreForSide(Score score, int side_sign);
int scoreAsLichessCp(Score score);
std::string formatScore(Score score);

std::string lichessJudgement(Score best, Score played);
double lichessAccuracy(Score best, Score played);
int lichessCpLoss(Score before_white, Score after_white, int mover_sign);
std::optional<int> lichessGameAccuracy(int side_sign,
                                       const std::vector<std::pair<int, Score>>& ply_scores);
