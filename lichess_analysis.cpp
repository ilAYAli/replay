#include "lichess_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include <fmt/format.h>

namespace {

int lichessCappedCp(int cp) {
    return std::clamp(cp, -1000, 1000);
}

double lichessWinningChances(int cp) {
    constexpr double multiplier = -0.00368208;
    double chances = 2.0 / (1.0 + std::exp(multiplier * lichessCappedCp(cp))) - 1.0;
    return std::clamp(chances, -1.0, 1.0);
}

std::string lichessJudgement(int best_cp, int played_cp) {
    double loss = lichessWinningChances(best_cp) - lichessWinningChances(played_cp);
    if (loss >= 0.3)
        return "blunder";
    if (loss >= 0.2)
        return "mistake";
    if (loss >= 0.1)
        return "inaccuracy";
    return "";
}

std::string lichessMateJudgement(Score best, Score played) {
    if (best.kind == ScoreKind::Cp
     && played.kind == ScoreKind::Mate
     && played.value < 0) {
        if (best.value < -999)
            return "inaccuracy";
        if (best.value < -700)
            return "mistake";
        return "blunder";
    }

    if (best.kind == ScoreKind::Mate
     && best.value > 0
     && (played.kind == ScoreKind::Cp
      || (played.kind == ScoreKind::Mate && played.value < 0))) {
        int played_cp_or_zero = played.kind == ScoreKind::Cp ? played.value : 0;
        if (played_cp_or_zero > 999)
            return "inaccuracy";
        if (played_cp_or_zero > 700)
            return "mistake";
        return "blunder";
    }

    return "";
}

double lichessWinPercent(int cp) {
    return 50.0 + 50.0 * lichessWinningChances(cp);
}

double lichessAccuracyFromWinPercents(double before, double after) {
    if (after >= before)
        return 100.0;

    double win_diff = before - after;
    double accuracy = 103.1668100711649 * std::exp(-0.04354415386753951 * win_diff)
                    - 3.166924740191411
                    + 1.0;
    return std::clamp(accuracy, 0.0, 100.0);
}

std::optional<double> standardDeviation(const std::vector<double>& values) {
    if (values.empty())
        return std::nullopt;

    double mean = 0.0;
    for (double value : values)
        mean += value;
    mean /= static_cast<double>(values.size());

    double variance = 0.0;
    for (double value : values)
        variance += (value - mean) * (value - mean);
    variance /= static_cast<double>(values.size());
    return std::sqrt(variance);
}

std::optional<double> weightedMean(const std::vector<std::pair<double, double>>& values) {
    if (values.empty())
        return std::nullopt;

    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    for (const auto& [value, weight] : values) {
        weighted_sum += value * weight;
        weight_sum += weight;
    }

    if (weight_sum == 0.0)
        return std::nullopt;
    return weighted_sum / weight_sum;
}

std::optional<double> harmonicMean(const std::vector<double>& values) {
    if (values.empty())
        return std::nullopt;

    double denominator = 0.0;
    for (double value : values)
        denominator += 1.0 / std::max(1.0, value);

    return static_cast<double>(values.size()) / denominator;
}

} // namespace

Score scoreForSide(Score score, int side_sign) {
    if (score.kind != ScoreKind::None)
        score.value *= side_sign;
    return score;
}

int scoreAsLichessCp(Score score) {
    if (score.kind == ScoreKind::Cp)
        return std::clamp(score.value, -1000, 1000);
    if (score.kind == ScoreKind::Mate)
        return score.value >= 0 ? 1000 : -1000;
    return 0;
}

std::string formatScore(Score score) {
    if (score.kind == ScoreKind::Cp)
        return score.value == 0 ? "0cp" : fmt::format("{:+}cp", score.value);
    if (score.kind == ScoreKind::Mate) {
        if (score.value >= 0)
            return fmt::format("M{}", score.value);
        return fmt::format("-M{}", std::abs(score.value));
    }
    return "n/a";
}

std::string lichessJudgement(Score best, Score played) {
    if (best.kind == ScoreKind::Cp && played.kind == ScoreKind::Cp)
        return lichessJudgement(best.value, played.value);
    return lichessMateJudgement(best, played);
}

double lichessAccuracy(Score best, Score played) {
    int best_cp = scoreAsLichessCp(best);
    int played_cp = scoreAsLichessCp(played);
    double win_diff = std::max(0.0, lichessWinningChances(best_cp) - lichessWinningChances(played_cp)) * 50.0;
    double accuracy = 103.1668100711649 * std::exp(-0.04354415386753951 * win_diff)
                    - 3.166924740191411
                    + 1.0;
    return std::clamp(accuracy, 0.0, 100.0);
}

int lichessCpLoss(Score before_white, Score after_white, int mover_sign) {
    int before = scoreAsLichessCp(before_white);
    int after = scoreAsLichessCp(after_white);
    int pov_loss = (after - before) * (mover_sign == +1 ? -1 : +1);
    return std::max(0, pov_loss);
}

std::optional<int> lichessGameAccuracy(int side_sign,
                                       const std::vector<std::pair<int, Score>>& ply_scores) {
    if (side_sign == 0 || ply_scores.empty())
        return std::nullopt;

    std::map<int, Score> by_ply;
    for (const auto& [ply, score] : ply_scores) {
        if (ply > 0)
            by_ply[ply] = score;
    }
    if (by_ply.empty())
        return std::nullopt;

    int max_ply = by_ply.rbegin()->first;
    for (int ply = 1; ply <= max_ply; ++ply) {
        if (!by_ply.contains(ply))
            return std::nullopt;
    }

    std::vector<double> win_percents;
    win_percents.reserve(static_cast<size_t>(max_ply) + 1);
    win_percents.push_back(lichessWinPercent(15));
    for (int ply = 1; ply <= max_ply; ++ply)
        win_percents.push_back(lichessWinPercent(scoreAsLichessCp(by_ply[ply])));

    size_t window_size = static_cast<size_t>(std::clamp(max_ply / 10, 2, 8));
    size_t n = win_percents.size();
    std::vector<double> weights;
    weights.reserve(n > 0 ? n - 1 : 0);

    auto add_weight = [&](size_t start) {
        std::vector<double> window;
        window.reserve(window_size);
        for (size_t i = start; i < std::min(start + window_size, n); ++i)
            window.push_back(win_percents[i]);
        auto deviation = standardDeviation(window);
        if (deviation)
            weights.push_back(std::clamp(*deviation, 0.5, 12.0));
    };

    size_t repeated = std::min(window_size, n) > 1 ? std::min(window_size, n) - 2 : 0;
    for (size_t i = 0; i < repeated; ++i)
        add_weight(0);
    for (size_t start = 0; start + window_size <= n; ++start)
        add_weight(start);

    if (weights.size() != n - 1)
        return std::nullopt;

    std::vector<std::pair<double, double>> weighted_accuracies;
    std::vector<double> accuracies;
    for (size_t i = 0; i < n - 1; ++i) {
        int mover_sign = i % 2 == 0 ? +1 : -1;
        if (mover_sign != side_sign)
            continue;

        double before = mover_sign == +1 ? win_percents[i] : win_percents[i + 1];
        double after = mover_sign == +1 ? win_percents[i + 1] : win_percents[i];
        double accuracy = lichessAccuracyFromWinPercents(before, after);
        weighted_accuracies.push_back({accuracy, weights[i]});
        accuracies.push_back(accuracy);
    }

    auto weighted = weightedMean(weighted_accuracies);
    auto harmonic = harmonicMean(accuracies);
    if (!weighted || !harmonic)
        return std::nullopt;

    return std::clamp(static_cast<int>(std::lround((*weighted + *harmonic) / 2.0)), 0, 100);
}
