#include "validator.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

namespace {

int extractDepth(const std::string& line) {
    size_t depthPos = line.find("depth");
    if (depthPos == std::string::npos)
        return -1;

    size_t startPos = depthPos + 6;
    size_t spacePos = line.find(' ', startPos);
    if (spacePos == std::string::npos)
        return -1;

    return std::stoi(line.substr(startPos, spacePos - startPos));
}

std::string extractMove(const std::string& line) {
    size_t bestMovePos = line.find("bestmove ");
    if (bestMovePos == std::string::npos)
        return "";

    std::string move = line.substr(bestMovePos + 9);
    size_t spacePos = move.find(' ');
    if (spacePos != std::string::npos)
        move = move.substr(0, spacePos);
    return move;
}

int moveCountFromPosition(const std::string& position) {
    size_t moves_pos = position.find(" moves ");
    if (moves_pos == std::string::npos)
        return 0;

    size_t start = moves_pos + 7;
    if (start >= position.size())
        return 0;

    std::string moves = position.substr(start);
    return (int)std::count(moves.begin(), moves.end(), ' ') + 1;
}

int baseSideToMoveSign(const std::string& position) {
    std::string prefix = "position fen ";
    if (position.rfind(prefix, 0) != 0)
        return +1;

    size_t board_end = position.find(' ', prefix.size());
    if (board_end == std::string::npos || board_end + 1 >= position.size())
        return +1;

    return position[board_end + 1] == 'b' ? -1 : +1;
}

int sideToMoveSign(const std::string& position) {
    int sign = baseSideToMoveSign(position);
    if (moveCountFromPosition(position) % 2 != 0)
        sign = -sign;
    return sign;
}

std::string appendMoveToPosition(const std::string& position, const std::string& move) {
    if (move.empty() || move == "(none)")
        return position;
    if (position.find(" moves ") != std::string::npos)
        return position + " " + move;
    return position + " moves " + move;
}

int qualityFromLoss(int loss_cp, bool stockfish_best) {
    if (stockfish_best)
        return 99;
    if (loss_cp <= 0)
        return 95;
    if (loss_cp >= 1000)
        return 0;
    return std::clamp((int)std::lround(99.0 * std::exp(-loss_cp / 180.0)), 0, 99);
}

enum class ScoreKind {
    None,
    Cp,
    Mate
};

struct Score {
    ScoreKind kind = ScoreKind::None;
    int value = 0;
};

Score scoreForSide(Score score, int side_sign) {
    if (score.kind != ScoreKind::None)
        score.value *= side_sign;
    return score;
}

int scoreAsCp(Score score) {
    if (score.kind == ScoreKind::Cp)
        return score.value;
    if (score.kind == ScoreKind::Mate) {
        int sign = score.value >= 0 ? +1 : -1;
        return sign * (100000 - std::min(std::abs(score.value), 1000));
    }
    return 0;
}

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

std::string lichessJudgement(Score best, Score played) {
    if (best.kind == ScoreKind::Cp && played.kind == ScoreKind::Cp)
        return lichessJudgement(best.value, played.value);
    return lichessMateJudgement(best, played);
}

bool isJudgementLabel(const std::string& label) {
    return label == "inaccuracy" || label == "mistake" || label == "blunder";
}

class UciProcess {
    FILE* engine_in = nullptr;
    FILE* engine_out = nullptr;
    pid_t pid = -1;

public:
    explicit UciProcess(const std::string& engine_path) {
        int pipe_to_engine[2];
        int pipe_from_engine[2];

        if (pipe(pipe_to_engine) == -1 || pipe(pipe_from_engine) == -1)
            throw std::runtime_error("Failed to create pipes");

        pid = fork();
        if (pid == -1)
            throw std::runtime_error("Failed to fork");

        if (pid == 0) {
            dup2(pipe_to_engine[0], STDIN_FILENO);
            dup2(pipe_from_engine[1], STDOUT_FILENO);

            int devnull = open("/dev/null", O_WRONLY);
            dup2(devnull, STDERR_FILENO);
            close(devnull);

            close(pipe_to_engine[0]);
            close(pipe_to_engine[1]);
            close(pipe_from_engine[0]);
            close(pipe_from_engine[1]);

            execlp(engine_path.c_str(), engine_path.c_str(), nullptr);
            exit(1);
        }

        close(pipe_to_engine[0]);
        close(pipe_from_engine[1]);

        engine_in = fdopen(pipe_to_engine[1], "w");
        engine_out = fdopen(pipe_from_engine[0], "r");

        if (!engine_in || !engine_out)
            throw std::runtime_error("Failed to open streams");

        setbuf(engine_in, nullptr);
        setbuf(engine_out, nullptr);
    }

    ~UciProcess() {
        if (engine_in) {
            fprintf(engine_in, "quit\n");
            fclose(engine_in);
        }
        if (engine_out)
            fclose(engine_out);
        if (pid > 0)
            waitpid(pid, nullptr, 0);
    }

    void send(const std::string& cmd) {
        fprintf(engine_in, "%s\n", cmd.c_str());
    }

    bool waitReadable(int timeout_ms) {
        int fd = fileno(engine_out);
        struct pollfd pfd{fd, POLLIN, 0};
        int rc = poll(&pfd, 1, timeout_ms);
        return rc > 0 && (pfd.revents & POLLIN);
    }

    std::string readLine() {
        char buffer[8192];
        if (!fgets(buffer, sizeof(buffer), engine_out))
            return "";

        std::string line(buffer);
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        return line;
    }
};

void waitForUciToken(UciProcess& engine, const std::string& token) {
    while (true) {
        if (!engine.waitReadable(30000))
            throw std::runtime_error(fmt::format("Timed out waiting for {}", token));

        std::string line = engine.readLine();
        if (line.empty())
            continue;
        if (line == token)
            break;
    }
}

void initializeUciEngine(UciProcess& engine) {
    engine.send("uci");
    waitForUciToken(engine, "uciok");
}

void waitForReady(UciProcess& engine) {
    engine.send("isready");
    waitForUciToken(engine, "readyok");
}

struct SearchResult {
    std::string bestmove;
    int depth = 0;
    Score score_white;
    bool has_score = false;
};

void updateSearchScore(SearchResult& result, const std::string& line, int stm_sign) {
    if (line.find("info depth ") == std::string::npos)
        return;

    int depth = extractDepth(line);
    if (depth > result.depth)
        result.depth = depth;

    size_t mate_pos = line.find("score mate ");
    size_t cp_pos = line.find("score cp ");
    if (mate_pos != std::string::npos) {
        size_t start = mate_pos + 11;
        size_t end = line.find(' ', start);
        int mate = std::stoi(line.substr(start, end - start));
        result.score_white = {ScoreKind::Mate, mate * stm_sign};
        result.has_score = true;
    } else if (cp_pos != std::string::npos) {
        size_t start = cp_pos + 9;
        size_t end = line.find(' ', start);
        int cp = std::stoi(line.substr(start, end - start));
        result.score_white = {ScoreKind::Cp, cp * stm_sign};
        result.has_score = true;
    }
}

SearchResult searchDepth(UciProcess& engine, const std::string& position, int depth, const std::string& search_move = "") {
    SearchResult result;
    int stm_sign = sideToMoveSign(position);

    engine.send(position);
    if (search_move.empty())
        engine.send(fmt::format("go depth {}", std::max(1, depth)));
    else
        engine.send(fmt::format("go depth {} searchmoves {}", std::max(1, depth), search_move));

    while (true) {
        std::string line = engine.readLine();
        if (line.empty())
            throw std::runtime_error("Validator engine closed during search");

        updateSearchScore(result, line, stm_sign);
        if (line.find("bestmove ") == 0) {
            result.bestmove = extractMove(line);
            break;
        }
    }

    return result;
}

MoveValidation validateMove(UciProcess& validator, const std::string& position, const std::string& played_move, int depth) {
    MoveValidation validation;
    int search_depth = std::max(1, depth);

    if (played_move.empty() || played_move == "(none)") {
        validation.error = "no played move";
        return validation;
    }

    validator.send("ucinewgame");
    waitForReady(validator);

    SearchResult before = searchDepth(validator, position, search_depth);
    if (!before.has_score) {
        validation.error = "validator returned no score";
        return validation;
    }

    validation.best_move = before.bestmove;  // Store SF's preferred move

    bool stockfish_best = before.bestmove == played_move;
    if (stockfish_best) {
        validation.ok = true;
        validation.quality = 99;
        return validation;
    }

    SearchResult played = searchDepth(validator, appendMoveToPosition(position, played_move), search_depth);
    if (!played.has_score) {
        validation.error = "validator returned no score";
        return validation;
    }

    int mover_sign = sideToMoveSign(position);
    Score best_score = scoreForSide(before.score_white, mover_sign);
    Score played_score = scoreForSide(played.score_white, mover_sign);
    int best_cp = scoreAsCp(best_score);
    int played_cp = scoreAsCp(played_score);
    int loss_cp = std::max(0, best_cp - played_cp);

    validation.ok = true;
    validation.quality = qualityFromLoss(loss_cp, stockfish_best);
    validation.label = lichessJudgement(best_score, played_score);
    validation.cp_loss = loss_cp;  // Store the centipawn loss
    return validation;
}

struct ValidationTask {
    std::string position;
    std::string played_move;
    int depth = 1;
    std::promise<MoveValidation> result;
};

const char* qualityColor(double quality) {
    if (quality >= 95.0)
        return "\033[1;32m";
    if (quality >= 90.0)
        return "\033[32m";
    if (quality >= 80.0)
        return "\033[33m";
    if (quality >= 50.0)
        return "\033[38;5;208m";
    return "\033[31m";
}

}  // namespace

std::string formatQualityPercent(int quality, bool color) {
    if (!color)
        return fmt::format("{}%", quality);
    return fmt::format("{}{}%\033[0m", qualityColor(quality), quality);
}

std::string formatQualityPercent(double quality, bool color) {
    if (!color)
        return fmt::format("{:.1f}%", quality);
    return fmt::format("{}{:.1f}%\033[0m", qualityColor(quality), quality);
}

std::string formatValidation(const MoveValidation& validation, bool color) {
    if (!validation.ok)
        return " | SF n/a";

    std::string quality_str = formatQualityPercent(validation.quality, color);
    
    if (isJudgementLabel(validation.label)) {
        std::string sf_move = validation.best_move.empty() ? "" : fmt::format(", SF prefers {}", validation.best_move);
        return fmt::format(" | SF {} ({}{})", quality_str, validation.label, sf_move);
    }
    
    return fmt::format(" | SF {}", quality_str);
}

class ValidatorWorker::Impl {
    UciProcess validator;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<ValidationTask> tasks;
    bool stopping = false;

public:
    explicit Impl(const std::string& validator_path)
        : validator(validator_path) {
        initializeUciEngine(validator);
        validator.send("ucinewgame");
        waitForReady(validator);
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        cv.notify_one();
        if (worker.joinable())
            worker.join();
    }

    std::future<MoveValidation> submit(const std::string& position, const std::string& played_move, int depth) {
        ValidationTask task;
        task.position = position;
        task.played_move = played_move;
        task.depth = std::max(1, depth);
        auto future = task.result.get_future();

        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        }
        cv.notify_one();

        return future;
    }

private:
    void run() {
        while (true) {
            ValidationTask task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (stopping && tasks.empty())
                    break;
                task = std::move(tasks.front());
                tasks.pop_front();
            }

            try {
                task.result.set_value(validateMove(validator, task.position, task.played_move, task.depth));
            } catch (...) {
                task.result.set_exception(std::current_exception());
            }
        }
    }
};

ValidatorWorker::ValidatorWorker(const std::string& validator_path)
    : impl(std::make_unique<Impl>(validator_path)) {
}

ValidatorWorker::~ValidatorWorker() = default;

std::future<MoveValidation> ValidatorWorker::submit(const std::string& position, const std::string& played_move, int depth) {
    return impl->submit(position, played_move, depth);
}
