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
#include <string_view>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
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

constexpr const char* kSuppressLogTimeWarning = "REPLAY_SUPPRESS_LOG_TIME_WARNING";
constexpr const char* kReplayBatch = "REPLAY_BATCH";

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

struct EngineConfig {
    std::string hash;
    std::string nnue2_hash;
};

struct AnalysisCache {
    std::string key;
    std::string provenance;
};

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

std::string colorizeJudgementToken(const std::string& text, const std::string& label, bool color) {
    std::string ansi = judgementColor(label);
    if (!color || ansi.empty() || !startsWith(text, label))
        return text;
    return ansi + label + "\033[0m" + text.substr(label.size());
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

std::string formatAnalysisEntry(const AnalysisEntry& entry,
                                const AnalysisWidths& widths,
                                bool color) {
    std::string label = colorizeJudgementToken(fmt::format("{:<11}", entry.label + ":"),
                                               entry.label, color);
    std::string line = fmt::format("{} {:<{}}  best: {:<{}}  loss: {:>{}}",
                                   label, analysisPlayedText(entry), widths.played,
                                   entry.best, widths.best,
                                   fmt::format("{}cp", entry.cp_loss), widths.loss);
    if (!entry.fen.empty())
        line += "  FEN: " + entry.fen;
    return line;
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
                           colorizeJudgementToken(fmt::format("{:<{}}", judgement, judgement_width),
                                                  validation.label, color),
                           best_report);
    }

    std::string judgement = validation.reference_best
        ? fmt::format("best {}", validation.best_score)
        : fmt::format("loss {}cp", validation.cp_loss);
    std::string label = validation.reference_best ? "best" : "";

    return fmt::format("ref {}{}",
                       colorizeJudgementToken(fmt::format("{:<{}}", judgement, judgement_width), label, color),
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
            output += colorizeJudgementToken(line, "blunder", true);
        else if (startsWith(line, "mistake:"))
            output += colorizeJudgementToken(line, "mistake", true);
        else if (startsWith(line, "inaccuracy:"))
            output += colorizeJudgementToken(line, "inaccuracy", true);
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

void hashAppend(uint64_t& hash, std::string_view data) {
    constexpr uint64_t prime = 1099511628211ULL;
    for (unsigned char ch : data) {
        hash ^= ch;
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

std::optional<std::filesystem::path> resolveExecutablePath(const std::string& engine_path) {
    std::string resolved = engine_path;
    if (engine_path.find('/') == std::string::npos) {
        std::string command = "command -v " + shellQuote(engine_path);
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe) {
            char buffer[4096];
            if (fgets(buffer, sizeof(buffer), pipe))
                resolved = trim(buffer);
            pclose(pipe);
        }
    }

    auto path = expandPath(resolved);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::nullopt;

    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return canonical;
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
    details += fmt::format("{}={}", name, path.string());
    if (std::filesystem::is_regular_file(path, ec)) {
        std::string key = path.string();
        if (!file_hashes.contains(key))
            file_hashes[key] = hashFileContent(path, 16);
        details += fmt::format(" hash={}", file_hashes[key]);
        if (option.find("nnue2") != std::string::npos)
            nnue2_hash = file_hashes[key].substr(0, 8);
    } else {
        details += " hash=missing";
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

std::vector<std::string> effectiveSetoptions(const std::vector<std::string>& log_setoptions,
                                             int threads) {
    std::vector<std::string> setoptions = log_setoptions;
    if (threads > 0)
        setoptions.push_back(fmt::format("setoption name Threads value {}", threads));
    return setoptions;
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

void sendInitCommand(EngineProcess& engine,
                     const std::string& command,
                     bool show_init,
                     bool print_command = true) {
    if (show_init && print_command)
        fmt::print("> {}\n", command);
    engine.send(command);
}

bool shouldPrintInitLine(const std::string& line) {
    return !line.empty()
        && line != "uciok"
        && line != "readyok"
        && !startsWith(line, "id author ")
        && !startsWith(line, "option name ");
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
        if (show_init && shouldPrintInitLine(*line))
            fmt::print("{}\n", *line);
        if (*line == token)
            return;
    }
}

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
    auto executable_path = resolveExecutablePath(engine_path);
    std::string executable_text = executable_path ? executable_path->string() : engine_path;
    std::string executable_hash = executable_path ? hashFileContent(*executable_path, 16) : "missing";
    std::string config_hash = hashString(fmt::format(
        "path={}\nresolved={}\nbinary={}\nuci={}\nsetoptions={}\nfiles={}\n",
        engine_path, executable_text, executable_hash, uci_text, setoption_text, file_options.details));

    return {
        config_hash,
        file_options.nnue2_hash
    };
}

std::string analysisModeName(bool time_mode, int analysis_depth, const std::string& analysis_target) {
    std::string mode = analysis_target == "log"
        ? "log"
        : (time_mode ? "time" : "replay");
    if (analysis_depth == 20)
        return mode;
    if (analysis_depth == 0)
        return mode + "_log-depth";
    return fmt::format("{}_depth{}", mode, analysis_depth);
}

std::string analysisTargetSuffix(const std::string& analysis_target) {
    return analysis_target == "log" ? "log" : "rpl";
}

std::filesystem::path analysisPath(const std::filesystem::path& logfile,
                                   const std::string& analysis_key,
                                   const std::string& analysis_target) {
    return logfile.parent_path() / fmt::format("{}.{}_{}_analysis",
                                               logfile.stem().string(),
                                               analysis_key,
                                               analysisTargetSuffix(analysis_target));
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

AnalysisCache buildAnalysisCache(const std::filesystem::path& logfile,
                                 const EngineConfig& candidate,
                                 const EngineConfig& reference,
                                 bool time_mode,
                                 int analysis_depth,
                                 const std::string& analysis_target) {
    auto pgn_path = logfile;
    pgn_path.replace_extension(".pgn");

    std::string log_hash = hashFileContent(logfile);
    std::string pgn_hash = std::filesystem::exists(pgn_path) ? hashFileContent(pgn_path) : "none";
    std::string input_hash = hashString(fmt::format("log={}\npgn={}\n", log_hash, pgn_hash));
    std::string mode = analysisModeName(time_mode, analysis_depth, analysis_target);
    std::string mode_hash = hashString(fmt::format("mode={}\ntime={}\ntarget={}\ndepth={}\n",
                                                   mode, time_mode, analysis_target,
                                                   analysis_depth));

    std::string key = hashString(fmt::format(
        "replay-cache-v4\ncandidate={}\nreference={}\nlog={}\npgn={}\nmode={}\n",
        candidate.hash, reference.hash, log_hash, pgn_hash, mode_hash));

    std::string provenance = fmt::format(
        "analysis-key {} | candidate cfg {} | reference cfg {} | log {} | target {} | ref-depth {} | nnue2 {}",
        key,
        candidate.hash,
        reference.hash,
        input_hash,
        analysis_target,
        analysis_depth,
        candidate.nnue2_hash);

    return {key, provenance};
}

void initializeEngine(EngineProcess& engine,
                      const std::vector<std::string>& setoptions,
                      int threads,
                      bool show_init) {
    if (show_init)
        fmt::print("\n=== Candidate UCI ===\n");

    sendInitCommand(engine, "uci", show_init, false);
    waitForInitToken(engine, "uciok", show_init);

    for (const auto& option : setoptions)
        sendInitCommand(engine, option, show_init);

    if (threads > 0)
        sendInitCommand(engine, fmt::format("setoption name Threads value {}", threads), show_init);

    sendInitCommand(engine, "ucinewgame", show_init, false);
    sendInitCommand(engine, "isready", show_init, false);
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

bool isPgnResultToken(std::string_view token) {
    return token == "1-0"
        || token == "0-1"
        || token == "1/2-1/2"
        || token == "1-1"
        || token == "*";
}

std::string stripPgnMoveNumber(std::string token) {
    size_t pos = 0;
    while (pos < token.size() && std::isdigit((unsigned char)token[pos]))
        pos++;

    if (pos == 0 || pos == token.size() || token[pos] != '.')
        return token;

    while (pos < token.size() && token[pos] == '.')
        pos++;

    return token.substr(pos);
}

bool isPgnMoveNumberOnly(std::string_view token) {
    bool saw_dot = false;
    for (char ch : token) {
        if (ch == '.') {
            saw_dot = true;
            continue;
        }
        if (!std::isdigit((unsigned char)ch))
            return false;
    }
    return saw_dot;
}

std::optional<int> pgnMainlinePlyCount(const std::filesystem::path& pgn_path) {
    if (!std::filesystem::exists(pgn_path))
        return std::nullopt;

    std::string text = readFile(pgn_path);
    int ply_count = 0;
    int variation_depth = 0;
    bool in_brace = false;
    bool in_tag = false;
    bool in_semicolon = false;
    bool line_start = true;
    std::string token;

    auto flush_token = [&] {
        if (token.empty())
            return;

        std::string move = stripPgnMoveNumber(token);
        while (!move.empty() && (move.back() == '!' || move.back() == '?'))
            move.pop_back();

        if (!move.empty()
         && move.front() != '$'
         && !isPgnResultToken(move)
         && !isPgnMoveNumberOnly(move))
            ply_count++;

        token.clear();
    };

    for (char ch : text) {
        if (in_semicolon) {
            if (ch == '\n') {
                in_semicolon = false;
                line_start = true;
            }
            continue;
        }

        if (in_tag) {
            if (ch == ']')
                in_tag = false;
            if (ch == '\n')
                line_start = true;
            continue;
        }

        if (in_brace) {
            if (ch == '}')
                in_brace = false;
            continue;
        }

        if (std::isspace((unsigned char)ch)) {
            flush_token();
            if (ch == '\n')
                line_start = true;
            continue;
        }

        if (line_start && ch == '[') {
            flush_token();
            in_tag = true;
            continue;
        }

        line_start = false;

        if (ch == ';') {
            flush_token();
            in_semicolon = true;
            continue;
        }

        if (ch == '{') {
            flush_token();
            in_brace = true;
            continue;
        }

        if (ch == '(') {
            flush_token();
            variation_depth++;
            continue;
        }

        if (ch == ')') {
            flush_token();
            if (variation_depth > 0)
                variation_depth--;
            continue;
        }

        if (variation_depth > 0)
            continue;

        token.push_back(ch);
    }

    flush_token();
    return ply_count;
}

void trimPostGameEntriesFromSiblingPgn(std::vector<LogEntry>& entries,
                                       const std::filesystem::path& logfile) {
    auto pgn_path = logfile;
    pgn_path.replace_extension(".pgn");

    auto ply_count = pgnMainlinePlyCount(pgn_path);
    if (!ply_count || *ply_count <= 0)
        return;

    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&](const LogEntry& entry) {
            return moveCountFromPosition(entry.position) >= *ply_count;
        }), entries.end());
}

int runDirectory(const std::filesystem::path& directory,
                 int argc,
                 char* argv[],
                 int logfile_arg_index) {
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".log")
            continue;
        logs.push_back(entry.path());
    }
    std::sort(logs.begin(), logs.end());

    if (logs.empty()) {
        fmt::print(stderr, "ERROR: No .log files found in {}\n", directory.string());
        return 1;
    }

    int failures = 0;
    setenv(kReplayBatch, "1", 1);
    for (size_t i = 0; i < logs.size(); ++i) {
        fmt::print("\n[{}/{}] {}\n", i + 1, logs.size(), logs[i].filename().string());
        std::fflush(stdout);

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
    std::string analysis_target = "replay";
    bool time_mode = false;
    bool analyze = true;
    bool verbose = false;
    bool color_output = true;
    bool force = false;
    bool engine_path_explicit = false;
    std::vector<std::pair<int, std::string>> positional_args;

    auto print_help = [&](const char* prog) {
        fmt::print(
            "Usage: {} [options] [engine] <logfile-or-directory>\n"
            "\n"
            "Replay UCI log searches and compare engine bestmoves with the log.\n"
            "At the end, analyze replay or log moves with a reference engine.\n"
            "Full reports are saved as <log>.<analysis-key>_<target>_analysis and reused.\n"
            "\n"
            "Options:\n"
            "  --engine <path>     Engine binary to replay with (default: enyo)\n"
            "  --candidate <path>  Alias for --engine\n"
            "  --reference <path>  Reference engine for blunder analysis (default: stockfish)\n"
            "  --ref-depth N       Reference analysis depth (default: 20; 0 follows logged depth)\n"
            "  --analyse T         Analyze replay or log moves: replay, log (default: replay)\n"
            "  --no-analysis       Replay only; do not run reference analysis\n"
            "  --move N            Start at fullmove N\n"
            "  --count N           Replay at most N logged engine moves\n"
            "  --time              Replay with the original logged go wtime/btime command\n"
            "  --threads N         Send `setoption name Threads value N`\n"
            "  --force             Ignore existing analysis files and analyze again\n"
            "  --no-color          Disable colored judgement output\n"
            "  --verbose, -v       Print full UCI traffic and cache hashes\n"
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
        } else if (arg == "--ref-depth" && i + 1 < argc) {
            analysis_depth = std::max(0, std::stoi(argv[++i]));
        } else if (arg == "--analyse" && i + 1 < argc) {
            analysis_target = argv[++i];
            if (analysis_target != "replay" && analysis_target != "log") {
                fmt::print(stderr, "ERROR: --analyse must be replay or log\n");
                return 1;
            }
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
                      && start_move == 0
                      && count < 0;

    bool logfile_is_directory = std::filesystem::is_directory(logfile);
    bool warn_log_time = analyze
                      && time_mode
                      && analysis_target == "log"
                      && std::getenv(kSuppressLogTimeWarning) == nullptr;
    if (warn_log_time) {
        fmt::print(stderr,
                   "WARNING: --time affects candidate replay only; "
                   "--analyse log analyzes fixed log moves.\n");
        if (logfile_is_directory)
            setenv(kSuppressLogTimeWarning, "1", 1);
    }

    if (logfile_is_directory)
        return runDirectory(logfile, argc, argv, logfile_arg_index);

    if (std::filesystem::path(logfile).extension() == ".pgn") {
        fmt::print(stderr, "ERROR: replay needs an Enyo .log file.\n");
        return 1;
    }

    try {
        std::filesystem::path logfile_path = logfile;
        std::filesystem::path report_path;
        ParsedLog parsed = readLog(logfile);
        std::vector<LogEntry> entries = parsed.entries;
        trimPostGameEntriesFromSiblingPgn(entries, logfile_path);
        if (entries.empty()) {
            fmt::print(stderr, "ERROR: No UCI go/bestmove pairs found in '{}'\n", logfile);
            return 1;
        }

        std::optional<AnalysisCache> cache;
        if (cache_enabled) {
            if (!executableExists(engine_path)) {
                fmt::print(stderr, "ERROR: Engine '{}' not found or not executable\n", engine_path);
                return 1;
            }
            if (!executableExists(reference_path)) {
                fmt::print(stderr, "ERROR: Reference engine '{}' not found or not executable\n", reference_path);
                fmt::print(stderr, "Use --reference <path> or --no-analysis.\n");
                return 1;
            }

            auto candidate_config = probeEngineConfig(engine_path, effectiveSetoptions(parsed.setoptions, threads));
            auto reference_config = probeEngineConfig(reference_path, {});
            cache = buildAnalysisCache(logfile_path, candidate_config, reference_config,
                                       time_mode, analysis_depth, analysis_target);
            report_path = analysisPath(logfile_path, cache->key, analysis_target);

            if (!force && std::filesystem::exists(report_path)) {
                bool batch_mode = std::getenv(kReplayBatch) != nullptr;
                std::string cached_report = readFile(report_path);
                std::string cached_body = cached_report;
                std::string cached_provenance;
                if (startsWith(cached_report, "analysis-key ")) {
                    size_t newline = cached_report.find('\n');
                    cached_provenance = cached_report.substr(0, newline);
                    cached_body = newline == std::string::npos ? "" : cached_report.substr(newline + 1);
                }

                if (!batch_mode)
                    fmt::print("Analysis reused: {}\n", report_path.string());
                if (verbose && !cached_provenance.empty())
                    fmt::print("{}\n", cached_provenance);
                if (!batch_mode)
                    fmt::print("=== Analysis ===\n");
                fmt::print("{}",
                           colorizeAnalysisReport(cached_body, color_output));
                if (cached_body.empty() || cached_body.back() != '\n')
                    fmt::print("\n");
                return 0;
            }
        }

        int total_entries = (int)entries.size();
        int display_total = entries.back().fullmove;
        fmt::print("Extracted {} go commands and {} bestmoves\n", total_entries, total_entries);
        if (verbose && cache)
            fmt::print("{}\n", cache->provenance);

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
                std::string analyzed_move = analysis_target == "log"
                    ? entry.expected
                    : result.bestmove;
                MoveValidation validation = validateMove(*reference,
                                                         entry.position,
                                                         analyzed_move,
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
                        analyzed_move,
                        analysis_target == "replay" && mismatch ? entry.expected : "",
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
            std::string analysis_report_body = formatAnalysisReport(report, analysis_failures, false);
            std::string analysis_report = cache
                ? cache->provenance + "\n" + analysis_report_body
                : analysis_report_body;
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
