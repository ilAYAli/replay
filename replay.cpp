#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <fmt/format.h>

#include "board.hpp"
#include "movegen.hpp"
#include "pgn.hpp"

struct LogEntry {
    std::string position;
    std::string logged_go;
    std::string replay_go;
    std::string expected;
    std::string fen;
    int fullmove = 1;
    int depth = 1;
};

struct ParsedLog {
    std::vector<std::string> setoptions;
    std::vector<LogEntry> entries;
};

struct SearchResult {
    std::string bestmove;
    double wdl = 0.0;
    int mate_in = 0;
};

enum class ScoreKind {
    None,
    Cp,
    Mate
};

struct Score {
    ScoreKind kind = ScoreKind::None;
    int value = 0;
};

struct ReferenceResult {
    std::string bestmove;
    Score score_white;
    bool has_score = false;
};

struct MoveValidation {
    bool ok = false;
    bool reference_best = false;
    std::string error;
    std::string label;
    std::string bestmove;
    std::string best_display;
    std::string best_score;
    int cp_loss = 0;
};

struct AnalysisEntry {
    std::string label;
    std::string played;
    std::string expected;
    std::string best;
    std::string fen;
    int fullmove = 1;
    int display_total = 1;
    int cp_loss = 0;
};

struct AnalysisWidths {
    size_t played = 4;
    size_t best = 4;
    size_t loss = 4;
};

struct ReplayLineWidths {
    size_t fullmove = 1;
    size_t replay = 18;
};

bool startsWith(const std::string& line, const std::string& prefix) {
    return line.rfind(prefix, 0) == 0;
}

std::string extractMove(const std::string& line) {
    if (!startsWith(line, "bestmove "))
        return "";

    size_t start = 9;
    size_t end = line.find(' ', start);
    return line.substr(start, end == std::string::npos ? end : end - start);
}

int parseIntField(const std::string& line, const std::string& key) {
    std::string needle = " " + key + " ";
    size_t pos = line.find(needle);
    size_t start = 0;
    if (pos == std::string::npos) {
        std::string head = key + " ";
        if (!startsWith(line, head))
            return -1;
        start = head.size();
    } else {
        start = pos + needle.size();
    }

    size_t end = line.find(' ', start);
    try {
        return std::stoi(line.substr(start, end == std::string::npos ? end : end - start));
    } catch (...) {
        return -1;
    }
}

int extractDepth(const std::string& line) {
    return parseIntField(line, "depth");
}

std::string formatMillis(int milliseconds) {
    if (milliseconds < 0)
        return "?";
    if (milliseconds >= 1000)
        return fmt::format("{:.1f}s", milliseconds / 1000.0);
    return fmt::format("{}ms", milliseconds);
}

std::string formatSearchProgress(const std::string& go_command, int current_depth) {
    int target_depth = parseIntField(go_command, "depth");
    if (target_depth > 0)
        return fmt::format("depth {}/{}", current_depth, target_depth);

    int wtime = parseIntField(go_command, "wtime");
    int btime = parseIntField(go_command, "btime");
    if (wtime >= 0 || btime >= 0)
        return fmt::format("depth {} | time w {} b {}",
                           current_depth, formatMillis(wtime), formatMillis(btime));

    return fmt::format("depth {}", current_depth);
}

int moveCountFromPosition(const std::string& position) {
    size_t moves_pos = position.find(" moves ");
    if (moves_pos == std::string::npos)
        return 0;

    std::istringstream stream(position.substr(moves_pos + 7));
    int count = 0;
    std::string move;
    while (stream >> move)
        count++;
    return count;
}

int fullmoveFromPosition(const std::string& position) {
    int fullmove = 1;
    char side = 'w';
    size_t moves_pos = position.find(" moves ");
    std::string head = moves_pos == std::string::npos
        ? position
        : position.substr(0, moves_pos);

    if (startsWith(head, "position fen ")) {
        std::istringstream fen(head.substr(13));
        std::string board, stm, castling, ep, halfmove, fullmove_text;
        fen >> board >> stm >> castling >> ep >> halfmove >> fullmove_text;
        if (stm == "b")
            side = 'b';
        if (!fullmove_text.empty())
            fullmove = std::stoi(fullmove_text);
    }

    for (int i = 0; i < moveCountFromPosition(position); ++i) {
        if (side == 'b')
            fullmove++;
        side = side == 'w' ? 'b' : 'w';
    }

    return fullmove;
}

int baseSideToMoveSign(const std::string& position) {
    if (!startsWith(position, "position fen "))
        return +1;

    size_t board_end = position.find(' ', 13);
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

std::string sideToMoveName(const std::string& position) {
    return sideToMoveSign(position) == +1 ? "White" : "Black";
}

bool applyUciMove(enyo::Board& board, const std::string& move) {
    auto resolved = enyo::uci_to_move(board, move);
    if (!resolved)
        return false;

    if (board.side == enyo::white)
        apply_move<enyo::white>(board, *resolved);
    else
        apply_move<enyo::black>(board, *resolved);
    return true;
}

std::optional<enyo::Board> boardFromPosition(const std::string& position) {
    std::string fen;
    std::string moves;
    if (startsWith(position, "position startpos")) {
        fen = "startpos";
        size_t moves_pos = position.find(" moves ");
        if (moves_pos != std::string::npos)
            moves = position.substr(moves_pos + 7);
    } else if (startsWith(position, "position fen ")) {
        std::string text = position.substr(13);
        size_t moves_pos = text.find(" moves ");
        fen = moves_pos == std::string::npos ? text : text.substr(0, moves_pos);
        if (moves_pos != std::string::npos)
            moves = text.substr(moves_pos + 7);
    } else {
        return std::nullopt;
    }

    try {
        enyo::Board board{fen};
        std::istringstream move_stream(moves);
        std::string move;
        while (move_stream >> move) {
            if (!applyUciMove(board, move))
                return std::nullopt;
        }
        return board;
    } catch (...) {
        return std::nullopt;
    }
}

std::string algebraForMove(const std::string& position, const std::string& move) {
    if (move.empty() || move == "(none)")
        return "";

    auto board = boardFromPosition(position);
    if (!board)
        return "";

    try {
        return enyo::uci_to_algebra(*board, move);
    } catch (...) {
        return "";
    }
}

std::string formatMoveWithAlgebra(const std::string& position, const std::string& move) {
    std::string algebra = algebraForMove(position, move);
    if (algebra.empty())
        return move;
    if (move.size() == 5 && algebra.rfind(move.substr(2, 2) + "=", 0) == 0)
        return move;
    return fmt::format("{} ({})", move, algebra);
}

ReplayLineWidths replayLineWidths(const std::vector<LogEntry>& entries, int display_total) {
    ReplayLineWidths widths;
    widths.fullmove = std::to_string(std::max(1, display_total)).size();
    for (const auto& entry : entries) {
        std::string expected = formatMoveWithAlgebra(entry.position, entry.expected);
        widths.replay = std::max(widths.replay, expected.size());
        widths.replay = std::max(widths.replay, fmt::format("{} != log {}", expected, expected).size());
    }
    return widths;
}

std::string appendMoveToPosition(const std::string& position, const std::string& move) {
    if (move.empty() || move == "(none)")
        return position;
    if (position.find(" moves ") != std::string::npos)
        return position + " " + move;
    return position + " moves " + move;
}

std::string extractSearchFen(const std::string& line) {
    size_t start = line.find("fen=");
    if (start == std::string::npos)
        return "";

    start += 4;
    size_t end = line.find(", legal_moves", start);
    return line.substr(start, end == std::string::npos ? end : end - start);
}

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

bool isReportableJudgement(const std::string& label) {
    return label == "inaccuracy" || label == "mistake" || label == "blunder";
}

std::string judgementColor(const std::string& label) {
    if (label == "blunder")
        return "\033[31m";
    if (label == "mistake")
        return "\033[38;5;208m";
    if (label == "inaccuracy")
        return "\033[33m";
    if (label == "best")
        return "\033[32m";
    return "";
}

std::string colorizeJudgement(const std::string& text, const std::string& label, bool color) {
    std::string ansi = judgementColor(label);
    if (!color || ansi.empty())
        return text;
    return ansi + text + "\033[0m";
}

std::string analysisPlayedText(const AnalysisEntry& entry) {
    size_t fullmove_width = std::to_string(std::max(1, entry.display_total)).size();
    std::string played = fmt::format("[{:>{}}/{}] {}",
                                     entry.fullmove, fullmove_width,
                                     entry.display_total, entry.played);
    if (!entry.expected.empty())
        played += fmt::format(" != log {}", entry.expected);
    return played;
}

AnalysisWidths analysisWidths(const std::vector<AnalysisEntry>& entries) {
    AnalysisWidths widths;
    for (const auto& entry : entries) {
        widths.played = std::max(widths.played, analysisPlayedText(entry).size());
        widths.best = std::max(widths.best, entry.best.size());
        widths.loss = std::max(widths.loss, fmt::format("{}cp", entry.cp_loss).size());
    }
    return widths;
}

std::string formatAnalysisEntry(const AnalysisEntry& entry, const AnalysisWidths& widths, bool color) {
    std::string line = fmt::format("{:<11} {:<{}}  best: {:<{}}  loss: {:>{}}  FEN: {}",
                                   entry.label + ":", analysisPlayedText(entry), widths.played,
                                   entry.best, widths.best,
                                   fmt::format("{}cp", entry.cp_loss), widths.loss,
                                   entry.fen);
    return colorizeJudgement(line, entry.label, color);
}

std::string formatReferenceInline(const MoveValidation& validation, bool color) {
    constexpr size_t judgement_width = 9;

    if (!validation.ok)
        return fmt::format("{:<{}}", "n/a", judgement_width);

    std::string best_suffix = validation.best_score.empty() ? "" : " " + validation.best_score;
    if (!validation.bestmove.empty() && validation.bestmove != "(none)") {
        best_suffix = fmt::format(" {}{}",
                                  validation.best_display.empty() ? validation.bestmove : validation.best_display,
                                  best_suffix);
    }
    std::string best_report = validation.reference_best && !best_suffix.empty()
        ? ""
        : " | best" + best_suffix;

    if (isReportableJudgement(validation.label)) {
        std::string judgement = fmt::format("{} {}cp", validation.label, validation.cp_loss);
        return fmt::format("ref {}{}",
                           colorizeJudgement(fmt::format("{:<{}}", judgement, judgement_width),
                                             validation.label, color),
                           best_report);
    }

    std::string judgement = validation.reference_best
        ? fmt::format("best {}", validation.best_score)
        : fmt::format("loss {}cp", validation.cp_loss);
    std::string label = validation.reference_best ? "best" : "";

    return fmt::format("ref {}{}",
                       colorizeJudgement(fmt::format("{:<{}}", judgement, judgement_width), label, color),
                       best_report);
}

std::string colorizeAnalysisReport(const std::string& report, bool color) {
    if (!color)
        return report;

    std::istringstream input(report);
    std::string output;
    std::string line;
    while (std::getline(input, line)) {
        if (startsWith(line, "blunder:"))
            output += colorizeJudgement(line, "blunder", true);
        else if (startsWith(line, "mistake:"))
            output += colorizeJudgement(line, "mistake", true);
        else if (startsWith(line, "inaccuracy:"))
            output += colorizeJudgement(line, "inaccuracy", true);
        else
            output += line;
        output += "\n";
    }

    return output;
}

std::string formatAnalysisReport(const std::vector<AnalysisEntry>& report,
                                 int analysis_failures,
                                 bool color) {
    std::string output;
    if (report.empty()) {
        output += "No inaccuracies, mistakes, or blunders.\n";
    } else {
        AnalysisWidths widths = analysisWidths(report);
        for (const auto& entry : report)
            output += formatAnalysisEntry(entry, widths, color) + "\n";
    }

    if (analysis_failures > 0)
        output += fmt::format("Analysis failures  : {}\n", analysis_failures);

    return output;
}

bool executableExists(const std::string& path) {
    if (path.find('/') != std::string::npos)
        return access(path.c_str(), X_OK) == 0;

    std::string check_cmd = fmt::format("which {} >/dev/null 2>&1", path);
    return system(check_cmd.c_str()) == 0;
}

bool isHashChar(char ch) {
    return std::isxdigit((unsigned char)ch) != 0;
}

std::string findHashTag(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!isHashChar(text[i]))
            continue;

        size_t start = i;
        while (i < text.size() && isHashChar(text[i]))
            ++i;

        size_t length = i - start;
        if (length < 7 || length > 40)
            continue;

        std::string tag = text.substr(start, length);
        if (text.find("dirty") != std::string::npos)
            tag += "-dirty";
        return tag;
    }

    return "";
}

std::string sanitizeTag(std::string tag) {
    for (char& ch : tag) {
        if (!std::isalnum((unsigned char)ch) && ch != '-' && ch != '_')
            ch = '-';
    }

    while (!tag.empty() && tag.front() == '-')
        tag.erase(tag.begin());
    while (!tag.empty() && tag.back() == '-')
        tag.pop_back();

    return tag.empty() ? "engine" : tag;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted += ch;
    }
    quoted += "'";
    return quoted;
}

class EngineProcess {
    FILE* engine_in = nullptr;
    FILE* engine_out = nullptr;
    pid_t pid = -1;
    bool verbose = false;

public:
    EngineProcess(const std::string& engine_path, bool verbose_output)
        : verbose(verbose_output) {
        int pipe_to_engine[2];
        int pipe_from_engine[2];

        if (pipe(pipe_to_engine) == -1 || pipe(pipe_from_engine) == -1)
            throw std::runtime_error("failed to create pipes");

        pid = fork();
        if (pid == -1)
            throw std::runtime_error("failed to fork");

        if (pid == 0) {
            dup2(pipe_to_engine[0], STDIN_FILENO);
            dup2(pipe_from_engine[1], STDOUT_FILENO);
            if (!verbose) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
            }
            signal(SIGPIPE, SIG_DFL);

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
            throw std::runtime_error("failed to open engine streams");

        setbuf(engine_in, nullptr);
        setbuf(engine_out, nullptr);
    }

    ~EngineProcess() {
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
        if (verbose)
            fmt::print("uci:> {}\n", cmd);
        if (fprintf(engine_in, "%s\n", cmd.c_str()) < 0 || fflush(engine_in) == EOF)
            throw std::runtime_error(fmt::format("engine stopped while sending command: {}", cmd));
    }

    bool waitReadable(int timeout_ms) {
        int fd = fileno(engine_out);
        struct pollfd pfd{fd, POLLIN, 0};
        int rc = poll(&pfd, 1, timeout_ms);
        return rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
    }

    std::optional<std::string> readLine() {
        char buffer[8192];
        if (!fgets(buffer, sizeof(buffer), engine_out))
            return std::nullopt;

        std::string line(buffer);
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        if (verbose)
            fmt::print("{}\n", line);
        return line;
    }

    bool hasExited() {
        if (pid <= 0)
            return true;

        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            pid = -1;
            return true;
        }
        return false;
    }
};

void waitForToken(EngineProcess& engine, const std::string& token) {
    while (true) {
        if (!engine.waitReadable(30000)) {
            if (engine.hasExited())
                throw std::runtime_error(fmt::format("engine exited while waiting for {}", token));
            throw std::runtime_error(fmt::format("timed out waiting for {}", token));
        }

        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error(fmt::format("engine closed stdout while waiting for {}", token));
        if (*line == token)
            return;
    }
}

void sendInitCommand(EngineProcess& engine, const std::string& command, bool show_init) {
    if (show_init)
        fmt::print("> {}\n", command);
    engine.send(command);
}

void waitForInitToken(EngineProcess& engine, const std::string& token, bool show_init) {
    while (true) {
        if (!engine.waitReadable(30000)) {
            if (engine.hasExited())
                throw std::runtime_error(fmt::format("engine exited while waiting for {}", token));
            throw std::runtime_error(fmt::format("timed out waiting for {}", token));
        }

        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error(fmt::format("engine closed stdout while waiting for {}", token));
        if (show_init)
            fmt::print("{}\n", *line);
        if (*line == token)
            return;
    }
}

std::string probeEngineTag(const std::string& engine_path) {
    EngineProcess engine(engine_path, false);
    engine.send("uci");

    std::string id_text;
    while (true) {
        if (!engine.waitReadable(30000)) {
            if (engine.hasExited())
                throw std::runtime_error("engine exited while waiting for uciok");
            throw std::runtime_error("timed out waiting for uciok");
        }

        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error("engine closed stdout while waiting for uciok");
        if (startsWith(*line, "id "))
            id_text += *line + " ";
        if (*line == "uciok")
            break;
    }

    std::string tag = findHashTag(id_text);
    if (!tag.empty())
        return sanitizeTag(tag);

    tag = findHashTag(std::filesystem::path(engine_path).filename().string());
    if (!tag.empty())
        return sanitizeTag(tag);

    return sanitizeTag(std::filesystem::path(engine_path).filename().string());
}

std::string analysisModeName(bool time_mode, int analysis_depth) {
    std::string mode = time_mode ? "time" : "replayed";
    if (analysis_depth == 20)
        return mode;
    if (analysis_depth == 0)
        return mode + "_logged-depth";
    return fmt::format("{}_depth{}", mode, analysis_depth);
}

std::filesystem::path analysisPath(const std::filesystem::path& logfile,
                                   const std::string& engine_tag,
                                   const std::string& mode) {
    return logfile.parent_path() / fmt::format("{}.{}_{}_analysis",
                                               logfile.stem().string(),
                                               engine_tag,
                                               mode);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error(fmt::format("failed to open {}", path.string()));

    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path);
    if (!file.is_open())
        throw std::runtime_error(fmt::format("failed to write {}", path.string()));

    file << text;
}

void initializeEngine(EngineProcess& engine,
                      const std::vector<std::string>& setoptions,
                      int threads,
                      bool show_init) {
    if (show_init)
        fmt::print("\n=== Candidate UCI ===\n");

    sendInitCommand(engine, "uci", show_init);
    waitForInitToken(engine, "uciok", show_init);

    for (const auto& option : setoptions)
        sendInitCommand(engine, option, show_init);

    if (threads > 0)
        sendInitCommand(engine, fmt::format("setoption name Threads value {}", threads), show_init);

    sendInitCommand(engine, "ucinewgame", show_init);
    sendInitCommand(engine, "isready", show_init);
    waitForInitToken(engine, "readyok", show_init);

    if (show_init)
        fmt::print("\n");
}

void resetReference(EngineProcess& engine) {
    engine.send("ucinewgame");
    engine.send("isready");
    waitForToken(engine, "readyok");
}

void initializeReference(EngineProcess& engine) {
    engine.send("uci");
    waitForToken(engine, "uciok");
    resetReference(engine);
}

void updateReferenceScore(ReferenceResult& result, const std::string& line, int stm_sign) {
    if (line.find("info depth ") == std::string::npos)
        return;

    size_t mate_pos = line.find("score mate ");
    size_t cp_pos = line.find("score cp ");
    if (mate_pos != std::string::npos) {
        size_t start = mate_pos + 11;
        size_t end = line.find(' ', start);
        int mate = std::stoi(line.substr(start, end == std::string::npos ? end : end - start));
        result.score_white = {ScoreKind::Mate, mate * stm_sign};
        result.has_score = true;
    } else if (cp_pos != std::string::npos) {
        size_t start = cp_pos + 9;
        size_t end = line.find(' ', start);
        int cp = std::stoi(line.substr(start, end == std::string::npos ? end : end - start));
        result.score_white = {ScoreKind::Cp, cp * stm_sign};
        result.has_score = true;
    }
}

ReferenceResult referenceSearch(EngineProcess& engine,
                                const std::string& position,
                                int depth,
                                bool progress,
                                const std::string& progress_text) {
    ReferenceResult result;
    int stm_sign = sideToMoveSign(position);
    int target_depth = std::max(1, depth);
    int last_reported_depth = -1;

    if (progress) {
        fmt::print("\r\033[K{} depth 0/{}", progress_text, target_depth);
        fflush(stdout);
    }

    resetReference(engine);
    engine.send(position);
    engine.send(fmt::format("go depth {}", target_depth));

    while (true) {
        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error("reference engine stopped during search");

        updateReferenceScore(result, *line, stm_sign);
        if (progress && line->find("info depth ") != std::string::npos) {
            int current_depth = extractDepth(*line);
            if (current_depth > 0 && current_depth != last_reported_depth) {
                last_reported_depth = current_depth;
                fmt::print("\r\033[K{} depth {}/{}", progress_text, current_depth, target_depth);
                fflush(stdout);
            }
        }
        if (startsWith(*line, "bestmove ")) {
            result.bestmove = extractMove(*line);
            return result;
        }
    }
}

MoveValidation validateMove(EngineProcess& reference,
                            const std::string& position,
                            const std::string& played_move,
                            int depth,
                            int fullmove,
                            int display_total,
                            bool progress) {
    MoveValidation validation;
    if (played_move.empty() || played_move == "(none)") {
        validation.error = "no played move";
        return validation;
    }

    ReferenceResult best = referenceSearch(reference,
                                          position,
                                          depth,
                                          progress,
                                          fmt::format("analyzing [{}/{}] reference-best",
                                                      fullmove, display_total));
    if (!best.has_score) {
        validation.error = "reference returned no score";
        return validation;
    }

    validation.bestmove = best.bestmove;
    int mover_sign = sideToMoveSign(position);
    Score best_score = scoreForSide(best.score_white, mover_sign);
    validation.best_display = formatMoveWithAlgebra(position, best.bestmove);
    validation.best_score = formatScore(best_score);

    validation.reference_best = best.bestmove == played_move;
    if (validation.reference_best) {
        validation.ok = true;
        return validation;
    }

    ReferenceResult played = referenceSearch(reference,
                                            appendMoveToPosition(position, played_move),
                                            depth,
                                            progress,
                                            fmt::format("analyzing [{}/{}] played-move",
                                                        fullmove, display_total));
    if (!played.has_score) {
        validation.error = "reference returned no score";
        return validation;
    }

    Score played_score = scoreForSide(played.score_white, mover_sign);
    int best_cp = scoreAsCp(best_score);
    int played_cp = scoreAsCp(played_score);

    validation.ok = true;
    validation.cp_loss = std::clamp(best_cp - played_cp, 0, 1000);
    validation.label = lichessJudgement(best_score, played_score);
    return validation;
}

SearchResult waitForBestmove(EngineProcess& engine,
                             const LogEntry& entry,
                             const std::string& go_command,
                             int display_total,
                             bool progress) {
    int score = 0;
    int mate_in = 0;
    double wdl = 0.0;
    int stm_sign = sideToMoveSign(entry.position);

    while (true) {
        auto line = engine.readLine();
        if (!line)
            break;
        if (line->empty())
            continue;

        if (line->find("info depth ") != std::string::npos) {
            int depth = extractDepth(*line);
            size_t mate_pos = line->find("score mate ");
            size_t score_pos = line->find("score cp ");

            if (mate_pos != std::string::npos) {
                size_t start = mate_pos + 11;
                size_t end = line->find(' ', start);
                mate_in = std::stoi(line->substr(start, end == std::string::npos ? end : end - start));
                wdl = mate_in >= 0 ? stm_sign : -stm_sign;
            } else if (score_pos != std::string::npos) {
                size_t start = score_pos + 9;
                size_t end = line->find(' ', start);
                score = std::stoi(line->substr(start, end == std::string::npos ? end : end - start));
                mate_in = 0;
                wdl = std::tanh((score * stm_sign) / 300.0);
            }

            if (progress) {
                fmt::print("\r\033[K{} thinking [{}/{}] {} | WDL {:+.2f} | expecting {}",
                           sideToMoveName(entry.position), entry.fullmove, display_total,
                           formatSearchProgress(go_command, depth), wdl, entry.expected);
                fflush(stdout);
            }
        }

        if (startsWith(*line, "bestmove ")) {
            if (progress)
                fmt::print("\r\033[K");
            return {extractMove(*line), wdl, mate_in};
        }
    }

    throw std::runtime_error("engine stopped before bestmove");
}

ParsedLog readLog(const std::filesystem::path& logfile) {
    std::ifstream file(logfile);
    if (!file.is_open())
        throw std::runtime_error(fmt::format("failed to open logfile: {}", logfile.string()));

    std::streampos first_ucinewgame = -1;
    std::string line;
    while (std::getline(file, line)) {
        if (line == "ucinewgame") {
            first_ucinewgame = file.tellg();
            break;
        }
    }

    file.clear();
    if (first_ucinewgame == -1)
        file.seekg(0, std::ios::beg);
    else
        file.seekg(first_ucinewgame);

    ParsedLog parsed;
    std::string current_position;
    std::string pending_position;
    std::string pending_go;
    std::string pending_fen;
    int pending_depth = 0;
    bool waiting_for_bestmove = false;

    while (std::getline(file, line)) {
        if (startsWith(line, "setoption ")) {
            parsed.setoptions.push_back(line);
            continue;
        }

        if (startsWith(line, "position ")) {
            current_position = line;
            continue;
        }

        if (startsWith(line, "go ")) {
            if (!current_position.empty()) {
                pending_position = current_position;
                pending_go = line;
                pending_fen.clear();
                pending_depth = 0;
                waiting_for_bestmove = true;
            }
            continue;
        }

        if (!waiting_for_bestmove)
            continue;

        if (startsWith(line, "search_position start:"))
            pending_fen = extractSearchFen(line);

        if (line.find("info depth ") != std::string::npos)
            pending_depth = std::max(pending_depth, extractDepth(line));

        if (!startsWith(line, "bestmove "))
            continue;

        if (pending_depth <= 0)
            pending_depth = 1;

        parsed.entries.push_back({
            pending_position,
            pending_go,
            fmt::format("go depth {}", pending_depth),
            extractMove(line),
            pending_fen,
            fullmoveFromPosition(pending_position),
            pending_depth
        });

        waiting_for_bestmove = false;
        pending_position.clear();
        pending_go.clear();
        pending_fen.clear();
    }

    return parsed;
}

int runDirectory(const std::filesystem::path& directory,
                 int argc,
                 char* argv[],
                 int logfile_arg_index,
                 bool cache_enabled,
                 bool force,
                 const std::string& engine_tag,
                 const std::string& analysis_mode) {
    std::vector<std::filesystem::path> logs;
    int cached = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".log")
            continue;

        if (cache_enabled && !force && std::filesystem::exists(analysisPath(entry.path(), engine_tag, analysis_mode))) {
            cached++;
            continue;
        }

        logs.push_back(entry.path());
    }
    std::sort(logs.begin(), logs.end());

    if (logs.empty()) {
        if (cached > 0) {
            fmt::print("All matching analyses are up to date ({} cached).\n", cached);
            return 0;
        }

        fmt::print(stderr, "ERROR: No .log files found in {}\n", directory.string());
        return 1;
    }

    if (cached > 0)
        fmt::print("Skipping {} cached logs\n", cached);

    int failures = 0;
    for (size_t i = 0; i < logs.size(); ++i) {
        fmt::print("[{}/{}] {}\n", i + 1, logs.size(), logs[i].filename().string());

        std::string command = shellQuote(argv[0]);
        for (int arg = 1; arg < argc; ++arg) {
            if (arg == logfile_arg_index)
                command += " " + shellQuote(logs[i].string());
            else
                command += " " + shellQuote(argv[arg]);
        }

        int rc = std::system(command.c_str());
        if (rc != 0)
            failures++;
    }

    return failures == 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    std::string engine_path = "enyo";
    std::string reference_path = "stockfish";
    std::string logfile;
    int logfile_arg_index = -1;
    int start_move = 0;
    int count = -1;
    int threads = -1;
    int analysis_depth = 20;
    bool time_mode = false;
    bool analyze = true;
    bool print_only = false;
    bool verbose = false;
    bool color_output = true;
    bool force = false;
    bool engine_path_explicit = false;
    std::vector<std::pair<int, std::string>> positional_args;

    auto print_help = [&](const char* prog) {
        fmt::print(
            "Usage: {} [options] [engine] <logfile-or-directory>\n"
            "\n"
            "Replay Enyo UCI log searches and compare engine bestmoves with the log.\n"
            "At the end, analyze replayed candidate moves with a reference engine.\n"
            "Full reports are saved as <log>.<engine-tag>_<mode>_analysis and reused.\n"
            "\n"
            "Options:\n"
            "  --engine <path>     Engine binary to replay with (default: enyo)\n"
            "  --candidate <path>  Alias for --engine\n"
            "  --reference <path>  Reference engine for blunder analysis (default: stockfish)\n"
            "  --analysis-depth N  Reference analysis depth (default: 20; 0 follows logged depth)\n"
            "  --no-analysis       Replay only; do not run reference analysis\n"
            "  --move N            Start at fullmove N\n"
            "  --count N           Replay at most N logged engine moves\n"
            "  --time              Replay with the original logged go wtime/btime command\n"
            "  --threads N         Send `setoption name Threads value N`\n"
            "  --print             Print logged bestmoves and exit\n"
            "  --force             Ignore existing analysis files and analyze again\n"
            "  --no-color          Disable colored judgement output\n"
            "  --verbose, -v       Print full UCI traffic\n"
            "  --help, -h          Show this help and exit\n",
            prog);
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if ((arg == "--engine" || arg == "--candidate") && i + 1 < argc) {
            engine_path = argv[++i];
            engine_path_explicit = true;
        } else if (arg == "--reference" && i + 1 < argc) {
            reference_path = argv[++i];
        } else if (arg == "--analysis-depth" && i + 1 < argc) {
            analysis_depth = std::max(0, std::stoi(argv[++i]));
        } else if (arg == "--no-analysis") {
            analyze = false;
        } else if (arg == "--move" && i + 1 < argc) {
            start_move = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::max(0, std::stoi(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--time") {
            time_mode = true;
        } else if (arg == "--print") {
            print_only = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--no-color") {
            color_output = false;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg.rfind("--", 0) == 0) {
            fmt::print(stderr, "Unknown or malformed option: {}\n", arg);
            return 1;
        } else {
            positional_args.push_back({i, arg});
        }
    }

    if (positional_args.size() == 1) {
        logfile_arg_index = positional_args[0].first;
        logfile = positional_args[0].second;
    } else if (positional_args.size() == 2 && !engine_path_explicit) {
        engine_path = positional_args[0].second;
        logfile_arg_index = positional_args[1].first;
        logfile = positional_args[1].second;
    } else if (positional_args.size() > 1) {
        fmt::print(stderr, "ERROR: Too many positional arguments\n");
        return 1;
    }

    if (logfile.empty()) {
        fmt::print(stderr, "ERROR: No logfile specified\n\n");
        print_help(argv[0]);
        return 1;
    }

    bool cache_enabled = analyze
                      && !print_only
                      && start_move == 0
                      && count < 0;
    std::string engine_tag;
    std::string analysis_mode;
    if (cache_enabled) {
        if (!executableExists(engine_path)) {
            fmt::print(stderr, "ERROR: Engine '{}' not found or not executable\n", engine_path);
            return 1;
        }

        try {
            engine_tag = probeEngineTag(engine_path);
        } catch (const std::exception& e) {
            fmt::print(stderr, "Error: {}\n", e.what());
            return 1;
        }
        analysis_mode = analysisModeName(time_mode, analysis_depth);
    }

    if (std::filesystem::is_directory(logfile))
        return runDirectory(logfile, argc, argv, logfile_arg_index,
                            cache_enabled, force, engine_tag, analysis_mode);

    if (std::filesystem::path(logfile).extension() == ".pgn") {
        fmt::print(stderr, "ERROR: replay needs an Enyo .log file.\n");
        return 1;
    }

    try {
        std::filesystem::path logfile_path = logfile;
        std::filesystem::path report_path;
        if (cache_enabled) {
            report_path = analysisPath(logfile_path, engine_tag, analysis_mode);
            if (!force && std::filesystem::exists(report_path)) {
                std::string cached_report = readFile(report_path);
                fmt::print("Analysis reused    : {}\n\n=== Analysis ===\n{}",
                           report_path.string(), colorizeAnalysisReport(cached_report, color_output));
                if (cached_report.empty() || cached_report.back() != '\n')
                    fmt::print("\n");
                return 0;
            }
        }

        ParsedLog parsed = readLog(logfile);
        std::vector<LogEntry> entries = parsed.entries;
        if (entries.empty()) {
            fmt::print(stderr, "ERROR: No UCI go/bestmove pairs found in '{}'\n", logfile);
            return 1;
        }

        int total_entries = (int)entries.size();
        int display_total = entries.back().fullmove;
        fmt::print("Extracted {} go commands and {} bestmoves\n", total_entries, total_entries);

        if (start_move > 0) {
            auto first = std::lower_bound(entries.begin(), entries.end(), start_move,
                [](const LogEntry& entry, int fullmove) {
                    return entry.fullmove < fullmove;
                });
            if (first == entries.end()) {
                fmt::print(stderr, "ERROR: --move {} leaves no matching or later positions\n", start_move);
                return 1;
            }
            int skipped = (int)std::distance(entries.begin(), first);
            entries.erase(entries.begin(), first);
            fmt::print("Starting at fullmove {}; skipped {} log entries; {} remaining\n",
                       entries.front().fullmove, skipped, entries.size());
        }

        if (count >= 0 && (int)entries.size() > count) {
            entries.erase(entries.begin() + count, entries.end());
            fmt::print("Limiting to {} moves\n", count);
        }

        if (print_only) {
            for (const auto& entry : entries)
                fmt::print("[{}/{}] {}\n", entry.fullmove, display_total, entry.expected);
            return 0;
        }
        ReplayLineWidths line_widths = replayLineWidths(entries, display_total);

        if (!executableExists(engine_path)) {
            fmt::print(stderr, "ERROR: Engine '{}' not found or not executable\n", engine_path);
            return 1;
        }

        std::unique_ptr<EngineProcess> reference;
        if (analyze) {
            if (!executableExists(reference_path)) {
                fmt::print(stderr, "ERROR: Reference engine '{}' not found or not executable\n", reference_path);
                fmt::print(stderr, "Use --reference <path> or --no-analysis.\n");
                return 1;
            }

            reference = std::make_unique<EngineProcess>(reference_path, verbose);
            initializeReference(*reference);
        }

        bool candidate_init_shown = false;
        auto make_engine = [&] {
            auto engine = std::make_unique<EngineProcess>(engine_path, verbose);
            bool show_init = !verbose && !candidate_init_shown;
            initializeEngine(*engine, parsed.setoptions, threads, show_init);
            candidate_init_shown = true;
            return engine;
        };

        std::unique_ptr<EngineProcess> engine = make_engine();

        int searched = 0;
        int mismatches = 0;
        double min_wdl = 0.0;
        double max_wdl = 0.0;
        double final_wdl = 0.0;
        bool progress = isatty(STDOUT_FILENO);
        std::vector<AnalysisEntry> report;
        int analysis_failures = 0;

        for (const auto& entry : entries) {
            if (!engine)
                engine = make_engine();

            std::string go_command = time_mode ? entry.logged_go : entry.replay_go;
            engine->send(entry.position);
            engine->send(go_command);

            SearchResult result = waitForBestmove(*engine, entry, go_command, display_total, progress);
            searched++;
            bool mismatch = result.bestmove != entry.expected;
            std::string played_display = formatMoveWithAlgebra(entry.position, result.bestmove);
            std::string expected_display = formatMoveWithAlgebra(entry.position, entry.expected);
            if (mismatch)
                mismatches++;
            min_wdl = std::min(min_wdl, result.wdl);
            max_wdl = std::max(max_wdl, result.wdl);
            final_wdl = result.wdl;

            std::string reference_suffix;
            if (analyze) {
                int depth = analysis_depth > 0 ? analysis_depth : entry.depth;
                MoveValidation validation = validateMove(*reference,
                                                         entry.position,
                                                         result.bestmove,
                                                         depth,
                                                         entry.fullmove,
                                                         display_total,
                                                         progress);
                if (progress)
                    fmt::print("\r\033[K");

                reference_suffix = formatReferenceInline(validation, color_output);
                if (!validation.ok) {
                    analysis_failures++;
                } else if (isReportableJudgement(validation.label)) {
                    report.push_back({
                        validation.label,
                        result.bestmove,
                        mismatch ? entry.expected : "",
                        validation.bestmove,
                        entry.fen,
                        entry.fullmove,
                        display_total,
                        validation.cp_loss
                    });
                }
            }

            std::string replay_display = mismatch
                ? fmt::format("{} != log {}", played_display, expected_display)
                : played_display;
            fmt::print("[{:>{}}/{}] {:<{}} | WDL {:+.2f} :: {}\n",
                       entry.fullmove, line_widths.fullmove, display_total,
                       replay_display, line_widths.replay,
                       result.wdl, reference_suffix);
            fflush(stdout);

            if (mismatch)
                engine.reset();
        }

        fmt::print("\n=== Summary ===\n");
        fmt::print("Positions replayed : {}\n", searched);
        fmt::print("Bestmove matches   : {}/{} ({} differed)\n",
                   searched - mismatches, searched, mismatches);
        fmt::print("Final WDL          : {:+.2f}\n", final_wdl);
        fmt::print("WDL range          : [{:+.2f}, {:+.2f}]\n", min_wdl, max_wdl);

        if (analyze) {
            std::string analysis_report = formatAnalysisReport(report, analysis_failures, false);
            fmt::print("\n=== Analysis ===\n{}",
                       formatAnalysisReport(report, analysis_failures, color_output));
            if (cache_enabled) {
                writeFile(report_path, analysis_report);
                fmt::print("Analysis saved     : {}\n", report_path.string());
            }
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }

    return 0;
}
