#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
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
    std::vector<std::string> diagnostics;
    int fullmove = 1;
    int depth = 1;
    long long nodes = 0;
};

struct ParsedLog {
    std::vector<std::string> setoptions;
    std::vector<LogEntry> entries;
};

constexpr const char* kSuppressLogTimeWarning = "REPLAY_SUPPRESS_LOG_TIME_WARNING";
constexpr const char* kReplayBatch = "REPLAY_BATCH";
constexpr const char* kBatchProgressPrefix = "BATCH_PROGRESS: ";
constexpr int kFishnetManualRequestNodes = 1'000'000;
constexpr int kFishnetChunkPositions = 6;
constexpr int kDefaultReferenceNodes = kFishnetManualRequestNodes * kFishnetChunkPositions
                                     / (kFishnetChunkPositions + 1);
constexpr long long kDefaultMaxReplayNodes = 100'000'000;

struct SearchResult {
    std::string bestmove;
    std::vector<std::string> diagnostics;
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

struct PositionSequence {
    std::string root;
    std::vector<std::string> moves;
};

struct PositionEvaluation {
    ReferenceResult result;
    bool ok = false;
    std::string error;
};

struct MoveValidation {
    bool ok = false;
    bool reference_best = false;
    std::string error;
    std::string label;
    std::string bestmove;
    std::string best_display;
    std::string best_score;
    Score before_score_white;
    Score after_score_white;
    int cp_loss = 0;
    double accuracy = 100.0;
    int ply_before = 0;
    int ply_after = 0;
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

struct AnalysisStats {
    int moves = 0;
    long long cp_loss = 0;
    double accuracy = 0.0;
    int side_sign = 0;
    std::vector<std::pair<int, Score>> ply_scores;
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

enum class ReferenceLimitKind {
    Nodes,
    Depth,
    LoggedDepth
};

struct ReferenceLimit {
    ReferenceLimitKind kind = ReferenceLimitKind::Nodes;
    int value = kDefaultReferenceNodes;
};

struct AnalysisCache {
    std::string key;
    std::string provenance;
};

struct FileOptionSummary {
    std::string details;
    std::string nnue2_hash = "none";
};

struct GameStatus {
    std::string result;
    std::string reason;
};

bool startsWith(const std::string& line, const std::string& prefix) {
    return line.rfind(prefix, 0) == 0;
}

std::string trim(std::string text);

bool isDiagnosticLine(const std::string& line) {
    return startsWith(line, "WARNING") || startsWith(line, "ERROR");
}

std::string stripDiagnosticFen(std::string line) {
    for (const std::string& marker : {" FEN: ", " fen="}) {
        size_t pos = line.find(marker);
        if (pos != std::string::npos)
            line.erase(pos);
    }
    return line;
}

std::string formatDiagnosticLine(const std::string& line, bool color, bool include_fen) {
    std::string output = include_fen ? line : stripDiagnosticFen(line);
    if (!color)
        return output;
    if (startsWith(output, "ERROR"))
        return "\033[31mERROR\033[0m" + output.substr(5);
    if (startsWith(output, "WARNING"))
        return "\033[33mWARNING\033[0m" + output.substr(7);
    return output;
}

std::string formatDiagnosticLine(const std::string& line,
                                 int fullmove,
                                 int display_total,
                                 bool color,
                                 bool include_fen) {
    std::string output = include_fen ? line : stripDiagnosticFen(line);
    std::string kind = startsWith(output, "ERROR") ? "ERROR" : "WARNING";
    std::string label = color
        ? (kind == "ERROR" ? "\033[31mERROR\033[0m" : "\033[33mWARNING\033[0m")
        : kind;
    std::string body = output.substr(kind.size());
    if (!body.empty() && body.front() == ':')
        body.erase(body.begin());
    body = trim(body);

    size_t fullmove_width = std::to_string(std::max(1, display_total)).size();
    return fmt::format("{}: [{:>{}}/{}]{}",
                       label, fullmove, fullmove_width, display_total,
                       body.empty() ? "" : " " + body);
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

std::string extractEmergencyMove(const std::string& line) {
    std::string normalized = lower(line);
    if (!startsWith(line, "EMERGENCY_MOVE:")
     && !startsWith(normalized, "warning: emergency_move")
     && !startsWith(normalized, "warning: emergency move"))
        return "";

    size_t start = line.find("move=");
    if (start == std::string::npos)
        return "";

    start += 5;
    size_t end = line.find(' ', start);
    return line.substr(start, end == std::string::npos ? end : end - start);
}

int parseIntField(const std::string& line, const std::string& key) {
    long long value = -1;
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
        value = std::stoll(line.substr(start, end == std::string::npos ? end : end - start));
    } catch (...) {
        return -1;
    }

    if (value < 0 || value > std::numeric_limits<int>::max())
        return -1;
    return (int)value;
}

long long parseLongField(const std::string& line, const std::string& key) {
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
        return std::stoll(line.substr(start, end == std::string::npos ? end : end - start));
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

std::string formatNodeCount(long long nodes) {
    if (nodes >= 1'000'000'000)
        return fmt::format("{:.1f}B", static_cast<double>(nodes) / 1'000'000'000.0);
    if (nodes >= 1'000'000)
        return fmt::format("{:.1f}M", static_cast<double>(nodes) / 1'000'000.0);
    if (nodes >= 1'000)
        return fmt::format("{:.1f}K", static_cast<double>(nodes) / 1'000.0);
    return fmt::format("{}", nodes);
}

std::string replayGoCommand(const std::string& logged_go,
                            long long logged_nodes,
                            long long max_replay_nodes) {
    if (logged_nodes <= 0)
        return logged_go;
    long long nodes = max_replay_nodes > 0
        ? std::min(logged_nodes, max_replay_nodes)
        : logged_nodes;
    return fmt::format("go nodes {}", nodes);
}

std::string formatSearchProgress(const std::string& go_command,
                                 int current_depth,
                                 long long current_nodes) {
    long long target_nodes = parseLongField(go_command, "nodes");
    if (target_nodes > 0)
        return fmt::format("depth {} | nodes {}/{}",
                           current_depth,
                           formatNodeCount(std::max(0LL, current_nodes)),
                           formatNodeCount(target_nodes));

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

std::string formatBatchSearchProgress(const std::string& go_command,
                                      int fullmove,
                                      int display_total,
                                      int current_depth,
                                      long long current_nodes) {
    long long target_nodes = parseLongField(go_command, "nodes");
    if (target_nodes > 0)
        return fmt::format("[{}/{}] nodes {}/{} depth {}",
                           fullmove,
                           display_total,
                           formatNodeCount(std::max(0LL, current_nodes)),
                           formatNodeCount(target_nodes),
                           current_depth);

    return fmt::format("[{}/{}] {}", fullmove, display_total,
                       formatSearchProgress(go_command, current_depth, current_nodes));
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

int plyBeforeMove(const std::string& position, int fullmove) {
    if (sideToMoveSign(position) == +1)
        return std::max(0, (fullmove - 1) * 2);
    return std::max(0, fullmove * 2 - 1);
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

bool hasLegalMoves(enyo::Board& board) {
    if (board.side == enyo::white)
        return !generate_legal_moves<enyo::white, false, false>(board).empty();
    return !generate_legal_moves<enyo::black, false, false>(board).empty();
}

bool isInCheck(const enyo::Board& board) {
    if (board.side == enyo::white)
        return is_check<enyo::white>(board);
    return is_check<enyo::black>(board);
}

bool insufficientMaterial(const enyo::Board& board) {
    if (board.pt_bb[enyo::white][enyo::pawn] || board.pt_bb[enyo::black][enyo::pawn]
     || board.pt_bb[enyo::white][enyo::rook] || board.pt_bb[enyo::black][enyo::rook]
     || board.pt_bb[enyo::white][enyo::queen] || board.pt_bb[enyo::black][enyo::queen])
        return false;

    int bishops = enyo::count_bits(board.pt_bb[enyo::white][enyo::bishop])
                + enyo::count_bits(board.pt_bb[enyo::black][enyo::bishop]);
    int knights = enyo::count_bits(board.pt_bb[enyo::white][enyo::knight])
                + enyo::count_bits(board.pt_bb[enyo::black][enyo::knight]);
    int minors = bishops + knights;
    if (minors == 0)
        return true;
    if (minors == 1)
        return true;
    if (bishops == 2 && knights == 0) {
        enyo::bitboard_t bb = board.pt_bb[enyo::white][enyo::bishop]
                            | board.pt_bb[enyo::black][enyo::bishop];
        int first = enyo::pop_lsb(bb);
        int second = enyo::pop_lsb(bb);
        return ((first / 8 + first % 8) & 1) == ((second / 8 + second % 8) & 1);
    }

    return false;
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

std::optional<std::string> positionAfterMove(const std::string& position,
                                             const std::string& move) {
    auto board = boardFromPosition(position);
    if (!board)
        return std::nullopt;
    if (!applyUciMove(*board, move))
        return std::nullopt;
    if (startsWith(position, "position startpos")
     || startsWith(position, "position fen ")) {
        if (position.find(" moves ") != std::string::npos)
            return position + " " + move;
        return position + " moves " + move;
    }
    return fmt::format("position fen {}", board->fen());
}

std::vector<std::string> splitMoves(std::string_view text) {
    std::vector<std::string> moves;
    std::istringstream stream(std::string{text});
    std::string move;
    while (stream >> move)
        moves.push_back(move);
    return moves;
}

std::optional<PositionSequence> positionSequenceFromPosition(const std::string& position) {
    if (!startsWith(position, "position startpos")
     && !startsWith(position, "position fen "))
        return std::nullopt;

    size_t moves_pos = position.find(" moves ");
    if (moves_pos == std::string::npos)
        return PositionSequence{position, {}};

    return PositionSequence{
        position.substr(0, moves_pos),
        splitMoves(position.substr(moves_pos + 7))
    };
}

std::string positionFromSequence(const PositionSequence& sequence, int ply) {
    ply = std::clamp(ply, 0, static_cast<int>(sequence.moves.size()));
    std::string position = sequence.root;
    if (ply > 0) {
        position += " moves";
        for (int i = 0; i < ply; ++i)
            position += " " + sequence.moves[i];
    }
    return position;
}

std::vector<std::string> immediateCheckmateMoves(enyo::Board board);

std::optional<PositionSequence> reconstructedSequenceFromEntries(const std::vector<LogEntry>& entries) {
    if (entries.empty())
        return std::nullopt;

    auto sequence = positionSequenceFromPosition(entries.back().position);
    if (!sequence)
        return std::nullopt;

    const std::string& move = entries.back().expected;
    if (!move.empty() && move != "(none)") {
        auto board = boardFromPosition(entries.back().position);
        if (!board || !applyUciMove(*board, move))
            return std::nullopt;
        sequence->moves.push_back(move);
    }

    auto final_board = boardFromPosition(positionFromSequence(*sequence,
                                                              static_cast<int>(sequence->moves.size())));
    if (final_board) {
        auto mates = immediateCheckmateMoves(*final_board);
        std::sort(mates.begin(), mates.end());
        if (!mates.empty())
            sequence->moves.push_back(mates.front());
    }

    return sequence;
}

std::optional<LogEntry> logEntryFromSequenceMove(const PositionSequence& sequence, int ply_before) {
    if (ply_before < 0 || ply_before >= static_cast<int>(sequence.moves.size()))
        return std::nullopt;

    std::string position = positionFromSequence(sequence, ply_before);
    auto board = boardFromPosition(position);
    if (!board)
        return std::nullopt;

    LogEntry entry;
    entry.position = position;
    entry.expected = sequence.moves[ply_before];
    entry.fen = board->fen();
    entry.fullmove = fullmoveFromPosition(position);
    entry.depth = 1;
    return entry;
}

std::vector<LogEntry> sideEntriesFromSequence(const PositionSequence& sequence, int side_sign) {
    std::vector<LogEntry> entries;
    for (int ply = 0; ply < static_cast<int>(sequence.moves.size()); ++ply) {
        std::string position = positionFromSequence(sequence, ply);
        if (sideToMoveSign(position) != side_sign)
            continue;
        auto entry = logEntryFromSequenceMove(sequence, ply);
        if (entry)
            entries.push_back(*entry);
    }
    return entries;
}

std::optional<GameStatus> terminalStatusAfterMove(const LogEntry& entry) {
    auto board = boardFromPosition(entry.position);
    if (!board)
        return std::nullopt;
    if (!applyUciMove(*board, entry.expected))
        return std::nullopt;

    if (!hasLegalMoves(*board))
        return isInCheck(*board)
            ? std::optional<GameStatus>{{"won", "by checkmate"}}
            : std::optional<GameStatus>{{"drawn", "by stalemate"}};

    int halfmove_clock = board->half_moves + static_cast<int>(board->gamestate.half_moves);
    if (halfmove_clock >= 100)
        return GameStatus{"drawn", "by 50-move rule"};
    if (insufficientMaterial(*board))
        return GameStatus{"drawn", "by insufficient material"};

    return std::nullopt;
}

bool terminalAfterMove(const LogEntry& entry) {
    return terminalStatusAfterMove(entry).has_value();
}

std::vector<std::string> immediateCheckmateMoves(enyo::Board board) {
    std::vector<std::string> mates;
    if (board.side == enyo::white) {
        for (const auto& move : generate_legal_moves<enyo::white, false, false>(board)) {
            enyo::Board next = board;
            apply_move<enyo::white>(next, move);
            if (!hasLegalMoves(next) && isInCheck(next))
                mates.push_back(fmt::format("{}", move));
        }
    } else {
        for (const auto& move : generate_legal_moves<enyo::black, false, false>(board)) {
            enyo::Board next = board;
            apply_move<enyo::black>(next, move);
            if (!hasLegalMoves(next) && isInCheck(next))
                mates.push_back(fmt::format("{}", move));
        }
    }
    return mates;
}

bool opponentHasImmediateCheckmateAfterMove(const LogEntry& entry) {
    auto board = boardFromPosition(entry.position);
    if (!board)
        return false;
    if (!applyUciMove(*board, entry.expected))
        return false;

    return !immediateCheckmateMoves(*board).empty();
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
    if (label == "timeout")
        return "\033[31m";
    if (label == "lost")
        return "\033[31m";
    if (label == "drawn")
        return "\033[33m";
    if (label == "won")
        return "\033[32m";
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

std::string colorizeGameStatusLine(const std::string& line, bool color) {
    if (!color || !startsWith(line, "game:"))
        return line;

    size_t status = line.find_first_not_of(' ', 5);
    if (status == std::string::npos)
        return line;
    size_t end = line.find(' ', status);
    std::string label = line.substr(status, end == std::string::npos ? end : end - status);
    std::string ansi = judgementColor(label);
    if (ansi.empty())
        return line;

    return line.substr(0, status) + ansi + line.substr(status) + "\033[0m";
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
                                bool color,
                                bool include_fen) {
    std::string label = colorizeJudgementToken(fmt::format("{:<11}", entry.label + ":"),
                                               entry.label, color);
    std::string line = fmt::format("{} {:<{}}  best: {:<{}}  loss: {:>{}}",
                                   label, analysisPlayedText(entry), widths.played,
                                   entry.best, widths.best,
                                   fmt::format("{}cp", entry.cp_loss), widths.loss);
    if (include_fen && !entry.fen.empty())
        line += "  FEN: " + entry.fen;
    return line;
}

std::string formatReferenceInline(const MoveValidation& validation, bool color) {
    constexpr size_t judgement_width = 9;

    if (!validation.ok)
        return fmt::format("{:<{}}", "n/a", judgement_width);

    std::string best_report;
    if (!validation.reference_best
     && !validation.bestmove.empty()
     && validation.bestmove != "(none)") {
        best_report = fmt::format(" | best {}",
                                  validation.best_display.empty() ? validation.bestmove
                                                                  : validation.best_display);
    }

    if (isReportableJudgement(validation.label)) {
        std::string judgement = fmt::format("{} {}cp", validation.label, validation.cp_loss);
        return fmt::format("ref {}{}",
                           colorizeJudgementToken(fmt::format("{:<{}}", judgement, judgement_width),
                                                  validation.label, color),
                           best_report);
    }

    std::string judgement = validation.reference_best
        ? (validation.best_score.empty() ? "best" : fmt::format("best {}", validation.best_score))
        : (validation.cp_loss == 0 ? "equal" : fmt::format("loss {}cp", validation.cp_loss));
    std::string label = validation.reference_best || validation.cp_loss == 0 ? "best" : "";

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
        else if (startsWith(line, "timeout:"))
            output += colorizeJudgementToken(line, "timeout", true);
        else if (startsWith(line, "game:"))
            output += colorizeGameStatusLine(line, true);
        else
            output += line;
        output += "\n";
    }

    return output;
}

std::string stripFenFields(const std::string& report) {
    std::istringstream input(report);
    std::string output;
    std::string line;
    while (std::getline(input, line)) {
        size_t fen = line.find("  FEN: ");
        if (fen != std::string::npos)
            line.erase(fen);
        output += line + "\n";
    }

    if (!report.empty() && report.back() != '\n' && !output.empty())
        output.pop_back();
    return output;
}

std::string displayAnalysisReport(const std::string& report, bool color, bool include_fen) {
    std::string text = include_fen ? report : stripFenFields(report);
    std::istringstream input(text);
    std::string normalized;
    std::string line;
    while (std::getline(input, line)) {
        if (startsWith(line, "accuracy:") && line.find("avg loss:") != std::string::npos) {
            int accuracy = -1;
            int avg_loss = -1;
            if (std::sscanf(line.c_str(), "accuracy: %d%% avg loss: %dcp",
                            &accuracy, &avg_loss) == 2) {
                normalized += fmt::format("{:<11} {}%\n", "accuracy:", accuracy);
                normalized += fmt::format("{:<11} {}cp\n", "avg loss:", avg_loss);
                continue;
            }
        } else if (startsWith(line, "accuracy:")) {
            int accuracy = -1;
            if (std::sscanf(line.c_str(), "accuracy: %d%%", &accuracy) == 1) {
                normalized += fmt::format("{:<11} {}%\n", "accuracy:", accuracy);
                continue;
            }
        } else if (startsWith(line, "avg loss:")) {
            int avg_loss = -1;
            if (std::sscanf(line.c_str(), "avg loss: %dcp", &avg_loss) == 1) {
                normalized += fmt::format("{:<11} {}cp\n", "avg loss:", avg_loss);
                continue;
            }
        }
        normalized += line + "\n";
    }

    return colorizeAnalysisReport(normalized, color);
}

bool batchMode() {
    return std::getenv(kReplayBatch) != nullptr;
}

void printSearchProgress(const std::string& text) {
    if (batchMode()) {
        fmt::print("{}{}\n", kBatchProgressPrefix, text);
    } else {
        fmt::print("\r\033[K{}", text);
    }
    std::fflush(stdout);
}

std::string batchIndent(const std::string& text) {
    if (!batchMode())
        return text;

    std::string output;
    bool line_start = true;
    for (char ch : text) {
        if (line_start && ch != '\n')
            output += "  ";
        output += ch;
        line_start = ch == '\n';
    }
    return output;
}

void printBatchBlock(const std::string& text) {
    fmt::print("{}", batchIndent(text));
}

void printSummaryReport(const std::string& report, bool color, bool include_fen) {
    printBatchBlock(displayAnalysisReport(report, color, include_fen));
}

std::string formatDiagnosticsReport(const std::vector<LogEntry>& entries,
                                    int display_total,
                                    bool color,
                                    bool include_fen) {
    std::string output;
    for (const auto& entry : entries) {
        for (const auto& diagnostic : entry.diagnostics)
            output += formatDiagnosticLine(diagnostic, entry.fullmove, display_total,
                                           color, include_fen) + "\n";
    }
    return output;
}

std::string formatTimeoutReport(const std::vector<LogEntry>& entries, int display_total) {
    if (entries.empty())
        return "";

    const auto& entry = entries.back();
    size_t fullmove_width = std::to_string(std::max(1, display_total)).size();
    bool white = sideToMoveSign(entry.position) == +1;
    int clock = parseIntField(entry.logged_go, white ? "wtime" : "btime");
    if (clock < 0 || clock > 1)
        return "";
    if (terminalAfterMove(entry))
        return "";
    if (opponentHasImmediateCheckmateAfterMove(entry))
        return "";

    return fmt::format("{:<11} [{:>{}}/{}] {} clock reached {} on final move\n",
                       "timeout:",
                       entry.fullmove,
                       fullmove_width,
                       display_total,
                       white ? "White" : "Black",
                       formatMillis(clock));
}

std::string formatGameStatusReport(const std::vector<LogEntry>& entries,
                                   const std::string& timeout_report) {
    if (entries.empty())
        return "";
    if (!timeout_report.empty())
        return fmt::format("{:<11} {}\n", "game:", "lost on time");

    auto status = terminalStatusAfterMove(entries.back());
    if (!status)
        return "";

    std::string reason = status->reason.empty() ? "" : " " + status->reason;
    return fmt::format("{:<11} {}{}\n", "game:", status->result, reason);
}

bool sameLogEntry(const LogEntry& lhs, const LogEntry& rhs) {
    return lhs.position == rhs.position
        && lhs.logged_go == rhs.logged_go
        && lhs.expected == rhs.expected;
}

void printLogMoveLine(const LogEntry& entry,
                      int display_total,
                      const ReplayLineWidths& widths,
                      const MoveValidation& validation,
                      bool color,
                      bool include_fen) {
    std::string played_display = formatMoveWithAlgebra(entry.position, entry.expected);
    std::string reference_suffix = formatReferenceInline(validation, color);
    printBatchBlock(fmt::format("[{:>{}}/{}] {:<{}} :: {}\n",
                                entry.fullmove, widths.fullmove, display_total,
                                played_display, widths.replay,
                                reference_suffix));
    for (const auto& diagnostic : entry.diagnostics)
        printBatchBlock(formatDiagnosticLine(diagnostic, entry.fullmove, display_total,
                                             color, include_fen) + "\n");
    fflush(stdout);
}

std::string replaceDerivedReportLines(const std::string& report,
                                      const std::string& game_report,
                                      const std::string& timeout_report) {
    std::istringstream input(report);
    std::string output;
    std::string score_report;
    std::string line;
    while (std::getline(input, line)) {
        if ((!game_report.empty() || !timeout_report.empty())
         && line == "No inaccuracies, mistakes, or blunders.")
            continue;
        if (startsWith(line, "accuracy:")
         || startsWith(line, "Accuracy:")
         || startsWith(line, "avg loss:")
         || startsWith(line, "Avg loss:")) {
            score_report += line + "\n";
            continue;
        }
        if (startsWith(line, "game:"))
            continue;
        if (!startsWith(line, "timeout:"))
            output += line + "\n";
    }

    output += timeout_report;
    output += score_report;
    output += game_report;
    if (output.empty())
        output = "No inaccuracies, mistakes, or blunders.\n";
    return output;
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

std::optional<int> lichessGameAccuracy(const AnalysisStats& stats) {
    if (stats.side_sign == 0 || stats.ply_scores.empty())
        return std::nullopt;

    std::map<int, Score> by_ply;
    for (const auto& [ply, score] : stats.ply_scores) {
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
    win_percents.reserve(max_ply + 1);
    win_percents.push_back(lichessWinPercent(15));
    for (int ply = 1; ply <= max_ply; ++ply)
        win_percents.push_back(lichessWinPercent(scoreAsLichessCp(by_ply[ply])));

    int window_size = std::clamp(max_ply / 10, 2, 8);
    int n = (int)win_percents.size();
    std::vector<double> weights;
    weights.reserve(std::max(0, n - 1));

    auto add_weight = [&](int start) {
        std::vector<double> window;
        window.reserve(window_size);
        for (int i = start; i < std::min(start + window_size, n); ++i)
            window.push_back(win_percents[i]);
        auto deviation = standardDeviation(window);
        if (deviation)
            weights.push_back(std::clamp(*deviation, 0.5, 12.0));
    };

    int repeated = std::max(0, std::min(window_size, n) - 2);
    for (int i = 0; i < repeated; ++i)
        add_weight(0);
    for (int start = 0; start + window_size <= n; ++start)
        add_weight(start);

    if ((int)weights.size() != n - 1)
        return std::nullopt;

    std::vector<std::pair<double, double>> weighted_accuracies;
    std::vector<double> accuracies;
    for (int i = 0; i < n - 1; ++i) {
        int mover_sign = i % 2 == 0 ? +1 : -1;
        if (mover_sign != stats.side_sign)
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

std::string formatAccuracyReport(const AnalysisStats& stats) {
    if (stats.moves == 0)
        return "";

    int accuracy = lichessGameAccuracy(stats)
        .value_or(std::clamp(static_cast<int>(std::lround(stats.accuracy / stats.moves)), 0, 100));
    int avg_loss = static_cast<int>(std::lround(static_cast<double>(stats.cp_loss) / stats.moves));
    return fmt::format("{:<11} {}%\n{:<11} {}cp\n",
                       "accuracy:", accuracy,
                       "avg loss:", avg_loss);
}

std::string formatAnalysisReport(const std::vector<AnalysisEntry>& report,
                                 const AnalysisStats& stats,
                                 int analysis_failures,
                                 bool color,
                                 bool include_fen,
                                 const std::string& game_report = "",
                                 const std::string& timeout_report = "") {
    std::string output;
    if (report.empty() && game_report.empty() && timeout_report.empty()) {
        output += "No inaccuracies, mistakes, or blunders.\n";
    } else if (!report.empty()) {
        AnalysisWidths widths = analysisWidths(report);
        for (const auto& entry : report)
            output += formatAnalysisEntry(entry, widths, color, include_fen) + "\n";
    }

    if (analysis_failures > 0)
        output += fmt::format("Analysis failures  : {}\n", analysis_failures);
    output += timeout_report;
    output += formatAccuracyReport(stats);
    output += game_report;

    return output;
}

bool reportTriggersFailure(const std::string& report) {
    std::istringstream input(report);
    std::string line;
    while (std::getline(input, line)) {
        if (startsWith(line, "blunder:") || startsWith(line, "timeout:"))
            return true;
    }

    return false;
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

std::filesystem::path tempOutputPath(size_t index) {
    auto directory = std::filesystem::temp_directory_path();
    return directory / fmt::format("replay-job-{}-{}.out", getpid(), index);
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

    std::optional<std::string> readLine(bool print_diagnostics = true) {
        char buffer[8192];
        if (!fgets(buffer, sizeof(buffer), engine_out))
            return std::nullopt;

        std::string line(buffer);
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        if (verbose)
            fmt::print("{}\n", line);
        else if (print_diagnostics && isDiagnosticLine(line))
            fmt::print(stderr, "{}\n", formatDiagnosticLine(line, false, false));
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

ReferenceLimit resolveReferenceLimit(const ReferenceLimit& limit, int logged_depth) {
    if (limit.kind != ReferenceLimitKind::LoggedDepth)
        return limit;

    return {ReferenceLimitKind::Depth, std::max(1, logged_depth)};
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

std::string analysisModeName(bool time_mode, const ReferenceLimit& reference_limit, const std::string& analysis_target) {
    std::string mode = analysis_target == "log"
        ? "log"
        : (time_mode ? "time" : "replay_nodes");
    if (reference_limit.kind == ReferenceLimitKind::Nodes && reference_limit.value == kDefaultReferenceNodes)
        return mode;
    return fmt::format("{}_{}", mode, referenceLimitName(reference_limit));
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

void initializeEngine(EngineProcess& engine,
                      const std::vector<std::string>& setoptions,
                      int threads) {
    engine.send("uci");
    waitForToken(engine, "uciok");

    for (const auto& option : setoptions)
        engine.send(option);

    if (threads > 0)
        engine.send(fmt::format("setoption name Threads value {}", threads));

    engine.send("ucinewgame");
    engine.send("isready");
    waitForToken(engine, "readyok");
}

void initializeReference(EngineProcess& engine) {
    engine.send("uci");
    waitForToken(engine, "uciok");
}

void resetReference(EngineProcess& engine) {
    engine.send("ucinewgame");
    engine.send("isready");
    waitForToken(engine, "readyok");
}

void updateReferenceScore(ReferenceResult& result, const std::string& line, int stm_sign) {
    if (line.find("info depth ") == std::string::npos)
        return;
    if (line.find(" upperbound ") != std::string::npos || line.find(" lowerbound ") != std::string::npos)
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
                                const ReferenceLimit& limit,
                                bool progress,
                                const std::string& progress_text,
                                const std::string& search_move = "",
                                bool reset = true) {
    ReferenceResult result;
    int stm_sign = sideToMoveSign(position);
    int target = std::max(1, limit.value);
    int last_reported_depth = -1;
    long long last_reported_nodes = -1;

    if (reset)
        resetReference(engine);

    if (progress) {
        if (limit.kind == ReferenceLimitKind::Nodes)
            fmt::print("\r\033[K{} nodes {}/{}", progress_text,
                       formatNodeCount(0), formatNodeCount(target));
        else
            fmt::print("\r\033[K{} depth 0/{}", progress_text, target);
        fflush(stdout);
    }

    engine.send(position);
    std::string go_limit = limit.kind == ReferenceLimitKind::Nodes
        ? fmt::format("nodes {}", target)
        : fmt::format("depth {}", target);
    if (search_move.empty())
        engine.send(fmt::format("go {}", go_limit));
    else
        engine.send(fmt::format("go {} searchmoves {}", go_limit, search_move));

    while (true) {
        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error("reference engine stopped during search");

        updateReferenceScore(result, *line, stm_sign);
        if (progress && line->find("info depth ") != std::string::npos) {
            if (limit.kind == ReferenceLimitKind::Nodes) {
                long long current_nodes = parseLongField(*line, "nodes");
                if (current_nodes > 0 && current_nodes != last_reported_nodes) {
                    last_reported_nodes = current_nodes;
                    fmt::print("\r\033[K{} nodes {}/{}", progress_text,
                               formatNodeCount(std::min(current_nodes,
                                                        static_cast<long long>(target))),
                               formatNodeCount(target));
                    fflush(stdout);
                }
            } else {
                int current_depth = extractDepth(*line);
                if (current_depth > 0 && current_depth != last_reported_depth) {
                    last_reported_depth = current_depth;
                    fmt::print("\r\033[K{} depth {}/{}", progress_text, current_depth, target);
                    fflush(stdout);
                }
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
                            const ReferenceLimit& limit,
                            int logged_depth,
                            int fullmove,
                            int display_total,
                            bool progress,
                            std::string_view progress_verb = "analyzing",
                            bool reset_reference = true) {
    MoveValidation validation;
    if (played_move.empty() || played_move == "(none)") {
        validation.error = "no played move";
        return validation;
    }

    ReferenceLimit resolved_limit = resolveReferenceLimit(limit, logged_depth);
    ReferenceResult best = referenceSearch(reference,
                                          position,
                                          resolved_limit,
                                          progress,
                                          fmt::format("{} [{}/{}] reference-best",
                                                      progress_verb, fullmove, display_total),
                                          "",
                                          reset_reference);
    if (!best.has_score) {
        validation.error = "reference returned no score";
        return validation;
    }

    validation.bestmove = best.bestmove;
    int mover_sign = sideToMoveSign(position);
    validation.before_score_white = best.score_white;
    Score best_score = scoreForSide(best.score_white, mover_sign);
    validation.best_display = formatMoveWithAlgebra(position, best.bestmove);
    validation.best_score = formatScore(best_score);
    validation.ply_before = plyBeforeMove(position, fullmove);
    validation.ply_after = validation.ply_before + 1;

    validation.reference_best = best.bestmove == played_move;

    auto played_position = positionAfterMove(position, played_move);
    if (!played_position) {
        validation.error = "could not apply played move";
        return validation;
    }

    ReferenceResult played = referenceSearch(reference,
                                            *played_position,
                                            resolved_limit,
                                            progress,
                                            fmt::format("{} [{}/{}] played-position",
                                                        progress_verb, fullmove, display_total),
                                            "",
                                            false);
    if (!played.has_score) {
        validation.error = "reference returned no score";
        return validation;
    }

    validation.after_score_white = played.score_white;
    Score played_score = scoreForSide(played.score_white, mover_sign);

    validation.ok = true;
    validation.cp_loss = lichessCpLoss(best.score_white, played.score_white, mover_sign);
    validation.accuracy = lichessAccuracy(best_score, played_score);
    validation.label = lichessJudgement(best_score, played_score);
    if (validation.cp_loss == 0 && isReportableJudgement(validation.label))
        validation.label.clear();
    return validation;
}

ReferenceLimit sequenceAnalysisLimit(const ReferenceLimit& limit) {
    if (limit.kind == ReferenceLimitKind::LoggedDepth)
        return ReferenceLimit{};
    return limit;
}

std::map<int, PositionEvaluation> analyzeFishnetPositionSequence(EngineProcess& reference,
                                                                 const PositionSequence& sequence,
                                                                 const ReferenceLimit& reference_limit,
                                                                 bool progress) {
    std::map<int, PositionEvaluation> evaluations;
    ReferenceLimit resolved_limit = sequenceAnalysisLimit(reference_limit);
    int max_ply = static_cast<int>(sequence.moves.size());
    if (max_ply < 0)
        return evaluations;

    std::vector<int> reversed;
    reversed.reserve(max_ply + 1);
    for (int ply = max_ply; ply >= 0; --ply)
        reversed.push_back(ply);

    int searched = 0;
    int total_searches = static_cast<int>(reversed.size())
                       + static_cast<int>((reversed.size() + kFishnetChunkPositions - 2)
                                          / (kFishnetChunkPositions - 1));

    for (int start = 0; start < static_cast<int>(reversed.size());
         start += kFishnetChunkPositions - 1) {
        int end = std::min(start + kFishnetChunkPositions - 1,
                           static_cast<int>(reversed.size()));
        int warmup_ply = start == 0 ? reversed[start] : reversed[start - 1];

        resetReference(reference);

        auto search_position = [&](int ply, bool store) {
            searched++;
            std::string position = positionFromSequence(sequence, ply);
            std::string progress_text = fmt::format("analyzing position {}/{}",
                                                    searched, total_searches);
            if (store)
                progress_text += fmt::format(" ply {}/{}", ply, max_ply);
            else
                progress_text += fmt::format(" warmup ply {}/{}", ply, max_ply);

            PositionEvaluation evaluation;
            try {
                evaluation.result = referenceSearch(reference,
                                                    position,
                                                    resolved_limit,
                                                    progress,
                                                    progress_text,
                                                    "",
                                                    false);
                if (!evaluation.result.has_score)
                    evaluation.error = "reference returned no score";
                else
                    evaluation.ok = true;
            } catch (const std::exception& ex) {
                evaluation.error = ex.what();
            }

            if (store)
                evaluations[ply] = evaluation;
        };

        search_position(warmup_ply, false);
        for (int index = start; index < end; ++index)
            search_position(reversed[index], true);
    }

    if (progress)
        fmt::print("\r\033[K");

    return evaluations;
}

MoveValidation validationFromPositionEvaluations(const LogEntry& entry,
                                                 const PositionEvaluation& before,
                                                 const PositionEvaluation& after) {
    MoveValidation validation;
    if (entry.expected.empty() || entry.expected == "(none)") {
        validation.error = "no played move";
        return validation;
    }
    if (!before.ok) {
        validation.error = before.error.empty() ? "reference returned no score" : before.error;
        return validation;
    }
    if (!after.ok) {
        validation.error = after.error.empty() ? "reference returned no score" : after.error;
        return validation;
    }

    int mover_sign = sideToMoveSign(entry.position);
    validation.bestmove = before.result.bestmove;
    validation.before_score_white = before.result.score_white;
    validation.after_score_white = after.result.score_white;
    validation.ply_before = plyBeforeMove(entry.position, entry.fullmove);
    validation.ply_after = validation.ply_before + 1;
    validation.reference_best = validation.bestmove == entry.expected;

    Score best_score = scoreForSide(validation.before_score_white, mover_sign);
    Score played_score = scoreForSide(validation.after_score_white, mover_sign);
    validation.best_display = formatMoveWithAlgebra(entry.position, validation.bestmove);
    validation.best_score = formatScore(best_score);
    validation.cp_loss = lichessCpLoss(validation.before_score_white,
                                       validation.after_score_white,
                                       mover_sign);
    validation.accuracy = lichessAccuracy(best_score, played_score);
    validation.label = lichessJudgement(best_score, played_score);
    if (validation.cp_loss == 0 && isReportableJudgement(validation.label))
        validation.label.clear();
    validation.ok = true;
    return validation;
}

void appendAnalysisEntry(std::vector<AnalysisEntry>& report,
                         AnalysisStats& stats,
                         int& analysis_failures,
                         const MoveValidation& validation,
                         const LogEntry& entry,
                         const std::string& analyzed_move,
                         const std::string& expected_move,
                         int display_total) {
    if (!validation.ok) {
        analysis_failures++;
        return;
    }

    stats.moves++;
    stats.cp_loss += validation.cp_loss;
    stats.accuracy += validation.accuracy;
    if (stats.side_sign == 0)
        stats.side_sign = sideToMoveSign(entry.position);
    if (validation.ply_before > 0)
        stats.ply_scores.push_back({validation.ply_before, validation.before_score_white});
    if (validation.ply_after > 0)
        stats.ply_scores.push_back({validation.ply_after, validation.after_score_white});

    if (!isReportableJudgement(validation.label))
        return;

    report.push_back({
        validation.label,
        analyzed_move,
        expected_move,
        validation.bestmove,
        entry.fen,
        entry.fullmove,
        display_total,
        validation.cp_loss
    });
}

void analyzeLoggedMoves(EngineProcess& reference,
                        const std::vector<LogEntry>& entries,
                        const ReferenceLimit& reference_limit,
                        int display_total,
                        const ReplayLineWidths& line_widths,
                        bool progress,
                        bool print_move_output,
                        bool color,
                        bool include_fen,
                        std::vector<AnalysisEntry>& report,
                        AnalysisStats& stats,
                        int& analysis_failures) {
    resetReference(reference);
    for (const auto& entry : entries) {
        MoveValidation validation = validateMove(reference,
                                                 entry.position,
                                                 entry.expected,
                                                 reference_limit,
                                                 entry.depth,
                                                 entry.fullmove,
                                                 display_total,
                                                 progress,
                                                 "analyzing",
                                                 false);
        if (progress)
            fmt::print("\r\033[K");
        if (print_move_output)
            printLogMoveLine(entry, display_total, line_widths,
                             validation, color, include_fen);
        appendAnalysisEntry(report, stats, analysis_failures, validation,
                             entry, entry.expected, "", display_total);
    }
}

bool analyzeLoggedPositionSequence(EngineProcess& reference,
                                   const std::vector<LogEntry>& source_entries,
                                   const std::vector<LogEntry>& requested_entries,
                                   bool full_log_range,
                                   const ReferenceLimit& reference_limit,
                                   int display_total,
                                   bool progress,
                                   bool print_move_output,
                                   bool color,
                                   bool include_fen,
                                   std::vector<AnalysisEntry>& report,
                                   AnalysisStats& stats,
                                   int& analysis_failures) {
    auto sequence = reconstructedSequenceFromEntries(source_entries);
    if (!sequence)
        return false;

    int side_sign = sideToMoveSign(requested_entries.front().position);
    std::vector<LogEntry> analyzed_entries = full_log_range
        ? sideEntriesFromSequence(*sequence, side_sign)
        : requested_entries;
    if (analyzed_entries.empty())
        return false;

    auto evaluations = analyzeFishnetPositionSequence(reference, *sequence,
                                                      reference_limit, progress);
    ReplayLineWidths line_widths = replayLineWidths(analyzed_entries, display_total);

    for (const auto& entry : analyzed_entries) {
        int ply_before = plyBeforeMove(entry.position, entry.fullmove);
        int ply_after = ply_before + 1;
        auto before = evaluations.find(ply_before);
        auto after = evaluations.find(ply_after);

        MoveValidation validation;
        if (before == evaluations.end()) {
            validation.error = fmt::format("missing reference evaluation for ply {}", ply_before);
        } else if (after == evaluations.end()) {
            validation.error = fmt::format("missing reference evaluation for ply {}", ply_after);
        } else {
            validation = validationFromPositionEvaluations(entry, before->second, after->second);
        }

        if (print_move_output)
            printLogMoveLine(entry, display_total, line_widths,
                             validation, color, include_fen);
        appendAnalysisEntry(report, stats, analysis_failures, validation,
                            entry, entry.expected, "", display_total);
    }

    return true;
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
    std::vector<std::string> diagnostics;

    while (true) {
        auto line = engine.readLine(false);
        if (!line)
            break;
        if (line->empty())
            continue;
        if (isDiagnosticLine(*line)) {
            diagnostics.push_back(*line);
            continue;
        }

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
                long long nodes = parseLongField(*line, "nodes");
                if (batchMode()) {
                    printSearchProgress(formatBatchSearchProgress(go_command,
                                                                  entry.fullmove,
                                                                  display_total,
                                                                  depth,
                                                                  nodes));
                } else {
                    printSearchProgress(fmt::format(
                        "{} thinking [{}/{}] {} | WDL {:+.2f} | expecting {}",
                        sideToMoveName(entry.position), entry.fullmove, display_total,
                        formatSearchProgress(go_command, depth, nodes),
                        wdl, entry.expected));
                }
            }
        }

        if (startsWith(*line, "bestmove ")) {
            if (progress)
                fmt::print("\r\033[K");
            return {extractMove(*line), diagnostics, wdl, mate_in};
        }
    }

    throw std::runtime_error("engine stopped before bestmove");
}

ParsedLog readLog(const std::filesystem::path& logfile,
                  long long max_replay_nodes) {
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
    std::string pending_fallback_move;
    std::vector<std::string> pending_diagnostics;
    int pending_depth = 0;
    long long pending_nodes = 0;
    bool waiting_for_bestmove = false;

    auto finish_pending_move = [&](const std::string& move) {
        if (pending_depth <= 0)
            pending_depth = 1;
        std::string replay_go = replayGoCommand(pending_go, pending_nodes,
                                                max_replay_nodes);
        if (pending_fen.empty()) {
            auto board = boardFromPosition(pending_position);
            if (board)
                pending_fen = board->fen();
        }

        parsed.entries.push_back({
            pending_position,
            pending_go,
            replay_go,
            move,
            pending_fen,
            pending_diagnostics,
            fullmoveFromPosition(pending_position),
            pending_depth,
            pending_nodes
        });

        waiting_for_bestmove = false;
        pending_position.clear();
        pending_go.clear();
        pending_fen.clear();
        pending_fallback_move.clear();
        pending_diagnostics.clear();
        pending_nodes = 0;
    };

    while (std::getline(file, line)) {
        if (startsWith(line, "setoption ")) {
            parsed.setoptions.push_back(line);
            continue;
        }

        if (startsWith(line, "position ")) {
            if (waiting_for_bestmove && !pending_fallback_move.empty())
                finish_pending_move(pending_fallback_move);
            current_position = line;
            continue;
        }

        if (startsWith(line, "go ")) {
            if (waiting_for_bestmove && !pending_fallback_move.empty())
                finish_pending_move(pending_fallback_move);
            if (!current_position.empty()) {
                pending_position = current_position;
                pending_go = line;
                pending_fen.clear();
                pending_fallback_move.clear();
                pending_diagnostics.clear();
                pending_depth = 0;
                pending_nodes = 0;
                waiting_for_bestmove = true;
            }
            continue;
        }

        if (!waiting_for_bestmove)
            continue;

        if (startsWith(line, "search_position start:"))
            pending_fen = extractSearchFen(line);

        if (startsWith(line, "info ")) {
            long long nodes = parseLongField(line, "nodes");
            if (nodes > 0)
                pending_nodes = nodes;
        }

        if (line.find("info depth ") != std::string::npos)
            pending_depth = std::max(pending_depth, extractDepth(line));

        if (isDiagnosticLine(line))
            pending_diagnostics.push_back(line);

        std::string move = extractMove(line);
        if (!move.empty()) {
            finish_pending_move(move);
            continue;
        }

        std::string fallback_move = extractEmergencyMove(line);
        if (!fallback_move.empty())
            pending_fallback_move = fallback_move;
    }

    if (waiting_for_bestmove && !pending_fallback_move.empty())
        finish_pending_move(pending_fallback_move);

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

bool containsArgIndex(const std::vector<int>& indices, int value) {
    return std::find(indices.begin(), indices.end(), value) != indices.end();
}

bool isLogTarget(const std::string& path) {
    return path == "-"
        || std::filesystem::path(path).extension() == ".log"
        || std::filesystem::is_directory(path);
}

bool isPositionalEngine(const std::string& first, const std::string& second) {
    if (isLogTarget(first) || !isLogTarget(second))
        return false;
    if (std::filesystem::exists(first))
        return executableExists(first);
    return true;
}

bool canBePositionalEngine(const std::string& path) {
    if (isLogTarget(path))
        return false;
    if (std::filesystem::exists(path))
        return executableExists(path);
    return true;
}

bool containsLogTarget(const std::vector<std::pair<int, std::string>>& args, size_t start) {
    for (size_t i = start; i < args.size(); ++i) {
        if (isLogTarget(args[i].second))
            return true;
    }
    return false;
}

void appendDirectoryLogs(const std::filesystem::path& directory,
                         std::vector<std::filesystem::path>& logs) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".log")
            continue;
        logs.push_back(entry.path());
    }
}

void appendStdinLogs(std::vector<std::filesystem::path>& logs) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        std::filesystem::path path = line;
        if (std::filesystem::is_directory(path)) {
            appendDirectoryLogs(path, logs);
        } else if (path.extension() == ".log") {
            logs.push_back(path);
        }
    }
}

std::vector<std::filesystem::path> collectLogTargets(const std::vector<std::string>& targets) {
    std::vector<std::filesystem::path> logs;
    for (const auto& target : targets) {
        if (target == "-") {
            appendStdinLogs(logs);
            continue;
        }
        std::filesystem::path path = target;
        if (std::filesystem::is_directory(path)) {
            appendDirectoryLogs(path, logs);
        } else if (path.extension() == ".log") {
            logs.push_back(path);
        }
    }
    std::sort(logs.begin(), logs.end());
    return logs;
}

bool interruptedStatus(int status) {
    if (status == -1)
        return false;
    if (WIFSIGNALED(status))
        return WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGQUIT;
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 128 + SIGINT || WEXITSTATUS(status) == 128 + SIGQUIT;
    return false;
}

std::string batchCommand(char* argv[],
                         int argc,
                         const std::vector<int>& logfile_arg_indices,
                         const std::filesystem::path& log,
                         const std::filesystem::path& output_path) {
    std::string command = shellQuote(argv[0]);
    bool inserted_log = false;
    for (int arg = 1; arg < argc; ++arg) {
        if (std::string(argv[arg]) == "--jobs" && arg + 1 < argc) {
            arg++;
            continue;
        }

        if (containsArgIndex(logfile_arg_indices, arg)) {
            if (!inserted_log) {
                command += " " + shellQuote(log.string());
                inserted_log = true;
            }
        } else {
            command += " " + shellQuote(argv[arg]);
        }
    }
    if (!inserted_log)
        command += " " + shellQuote(log.string());

    command += " > " + shellQuote(output_path.string()) + " 2>&1";
    return command;
}

struct RunningJob {
    pid_t pid = -1;
    size_t index = 0;
    std::filesystem::path output_path;
    std::chrono::steady_clock::time_point started;
};

pid_t startBatchJob(const std::string& command) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid;
}

void terminateJobs(const std::vector<RunningJob>& running) {
    for (const auto& job : running)
        kill(job.pid, SIGTERM);
    for (const auto& job : running)
        waitpid(job.pid, nullptr, 0);
}

void printJobOutput(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line)) {
        if (startsWith(line, kBatchProgressPrefix))
            continue;
        fmt::print("{}\n", line);
    }
}

std::string lastJobProgress(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return "";

    std::string line;
    std::string latest;
    while (std::getline(file, line)) {
        if (startsWith(line, kBatchProgressPrefix))
            latest = line.substr(std::string_view(kBatchProgressPrefix).size());
    }
    return latest;
}

void printBatchProgress(const std::vector<RunningJob>& running,
                        const std::vector<std::filesystem::path>& logs,
                        size_t completed) {
    auto oldest = running.front();
    for (const auto& job : running) {
        if (job.started < oldest.started)
            oldest = job;
    }

    std::string latest = lastJobProgress(oldest.output_path);
    fmt::print("progress: {}/{} done, {} running | {}{}\n",
               completed,
               logs.size(),
               running.size(),
               logs[oldest.index].filename().string(),
               latest.empty() ? "" : " | " + latest);
    std::fflush(stdout);
}

int waitForBatchJob(const RunningJob& job,
                    const std::vector<std::filesystem::path>& logs,
                    size_t completed) {
    auto last_progress = std::chrono::steady_clock::now();
    while (true) {
        int status = 0;
        pid_t done = waitpid(job.pid, &status, WNOHANG);
        if (done < 0) {
            if (errno == EINTR) {
                kill(job.pid, SIGTERM);
                waitpid(job.pid, nullptr, 0);
                return 128 + SIGINT;
            }
            throw std::runtime_error(fmt::format("waitpid failed: {}", strerror(errno)));
        }
        if (done == 0) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(10)) {
                printBatchProgress(std::vector<RunningJob>{job}, logs, completed);
                last_progress = now;
            }
            usleep(100000);
            continue;
        }

        return status;
    }
}

int runLogs(const std::vector<std::filesystem::path>& logs,
            int argc,
            char* argv[],
            const std::vector<int>& logfile_arg_indices,
            int jobs) {
    if (logs.empty()) {
        fmt::print(stderr, "ERROR: No .log files found\n");
        return 1;
    }
    if (logs.size() < 2)
        jobs = 1;

    int failures = 0;
    setenv(kReplayBatch, "1", 1);
    if (jobs <= 1) {
        for (size_t i = 0; i < logs.size(); ++i) {
            fmt::print("\n[{}/{}] {}\n", i + 1, logs.size(), logs[i].filename().string());
            std::fflush(stdout);

            auto output_path = tempOutputPath(i);
            std::string command = batchCommand(argv, argc, logfile_arg_indices,
                                               logs[i], output_path);
            pid_t pid = startBatchJob(command);
            if (pid < 0)
                throw std::runtime_error(fmt::format("failed to start job: {}", strerror(errno)));
            int rc = waitForBatchJob({pid, i, output_path, std::chrono::steady_clock::now()},
                                     logs, i);
            printJobOutput(output_path);
            std::filesystem::remove(output_path);
            if (rc == 128 + SIGINT || interruptedStatus(rc))
                return 128 + SIGINT;
            if (rc != 0)
                failures++;
        }
        return failures == 0 ? 0 : 1;
    }

    std::vector<RunningJob> running;
    std::vector<std::filesystem::path> output_paths(logs.size());
    size_t next = 0;
    size_t completed = 0;
    fmt::print("Running {} logs with {} jobs; output prints as each log finishes.\n",
               logs.size(), jobs);
    std::fflush(stdout);

    auto launch_next = [&] {
        auto output_path = tempOutputPath(next);
        std::string command = batchCommand(argv, argc, logfile_arg_indices,
                                           logs[next], output_path);
        pid_t pid = startBatchJob(command);
        if (pid < 0)
            throw std::runtime_error(fmt::format("failed to start job: {}", strerror(errno)));
        output_paths[next] = output_path;
        running.push_back({pid, next, output_path, std::chrono::steady_clock::now()});
        if (completed == 0) {
            fmt::print("analyzing [{}/{}] {}\n", next + 1, logs.size(), logs[next].filename().string());
            std::fflush(stdout);
        }
        next++;
    };
    auto print_finished = [&](const RunningJob& job, int status) {
        fmt::print("\n[{}/{}] {}\n",
                   job.index + 1, logs.size(), logs[job.index].filename().string());
        printJobOutput(job.output_path);
        std::filesystem::remove(job.output_path);
        std::fflush(stdout);

        if (interruptedStatus(status)) {
            terminateJobs(running);
            return false;
        }
        if (status != 0)
            failures++;
        completed++;
        return true;
    };

    try {
        while (next < logs.size() && running.size() < (size_t)jobs)
            launch_next();

        auto last_progress = std::chrono::steady_clock::now();
        while (!running.empty()) {
            int status = 0;
            pid_t done = waitpid(-1, &status, WNOHANG);
            if (done < 0) {
                if (errno == EINTR) {
                    terminateJobs(running);
                    return 128 + SIGINT;
                }
                throw std::runtime_error(fmt::format("waitpid failed: {}", strerror(errno)));
            }
            if (done == 0) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_progress >= std::chrono::seconds(10)) {
                    printBatchProgress(running, logs, completed);
                    last_progress = now;
                }
                usleep(100000);
                continue;
            }

            auto job = std::find_if(running.begin(), running.end(),
                [&](const RunningJob& candidate) {
                    return candidate.pid == done;
                });
            if (job == running.end())
                continue;

            RunningJob finished = *job;
            running.erase(job);
            if (!print_finished(finished, status))
                return 128 + SIGINT;
            if (next < logs.size()) {
                launch_next();
                printBatchProgress(running, logs, completed);
                last_progress = std::chrono::steady_clock::now();
            }
        }
    } catch (...) {
        terminateJobs(running);
        for (const auto& output_path : output_paths) {
            if (!output_path.empty())
                std::filesystem::remove(output_path);
        }
        throw;
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
    int jobs = 1;
    long long max_replay_nodes = kDefaultMaxReplayNodes;
    ReferenceLimit reference_limit;
    std::string analysis_target = "replay";
    bool time_mode = false;
    bool analyze = true;
    bool verbose = false;
    bool color_output = false;
    bool force = false;
    bool print_move_output = true;
    bool engine_path_explicit = false;
    std::vector<std::pair<int, std::string>> positional_args;

    auto print_help = [&](const char* prog) {
        fmt::print(
            "Usage: {} [options] [engine] <logfile-or-directory> [more logs...]\n"
            "\n"
            "Replay UCI log searches and compare engine output with logged moves.\n"
            "Candidate replay uses the logged final node count when available.\n"
            "Multiple log paths are treated as batch input; non-.log paths are ignored.\n"
            "Use '-' or pipe newline-separated paths on stdin to read log targets from stdin.\n"
            "At the end, analyze replayed moves with a reference engine.\n"
            "Replay reports are saved as <log>.<analysis-key>_analysis.\n"
            "Log reports are saved as <log>.analysis.\n"
            "\n"
            "Options:\n"
            "  --engine <path>     Engine binary to replay with (default: enyo)\n"
            "  --candidate <path>  Alias for --engine\n"
            "  --reference <path>  Reference engine for blunder analysis (default: stockfish)\n"
            "  --ref-nodes N       Reference analysis nodes; default uses 857142 nodes\n"
            "  --ref-depth N       Reference analysis depth instead of nodes; 0 follows logged depth\n"
            "  --log               Analyze logged moves instead of replayed moves;\n"
            "                      does not run the candidate engine\n"
            "  --no-analysis       Replay only; do not run reference analysis\n"
            "  --summary-only      Skip per-move output; print final summaries only\n"
            "  --move N            Start at fullmove N\n"
            "  --count N           Replay at most N logged engine moves\n"
            "  --max-replay-nodes N\n"
            "                      Cap candidate replay nodes (default: 100000000, 0 disables)\n"
            "  --time              Replay with the original logged go wtime/btime command;\n"
            "                      ignored with --log and overrides logged-node replay\n"
            "  --threads N         Send `setoption name Threads value N`\n"
            "  --jobs N            Run up to N logs in parallel in batch mode (default: 1)\n"
            "  --force             Ignore existing analysis files and analyze again\n"
            "  --color             Color judgement output\n"
            "  --verbose, -v       Print full UCI traffic, cache hashes, and FENs\n"
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
        } else if (arg == "--ref-nodes" && i + 1 < argc) {
            reference_limit = {ReferenceLimitKind::Nodes, std::max(1, std::stoi(argv[++i]))};
        } else if (arg == "--ref-depth" && i + 1 < argc) {
            int depth = std::max(0, std::stoi(argv[++i]));
            reference_limit = depth == 0
                ? ReferenceLimit{ReferenceLimitKind::LoggedDepth, 0}
                : ReferenceLimit{ReferenceLimitKind::Depth, depth};
        } else if (arg == "--log") {
            analysis_target = "log";
        } else if (arg == "--no-analysis") {
            analyze = false;
        } else if (arg == "--summary-only") {
            print_move_output = false;
        } else if (arg == "--move" && i + 1 < argc) {
            start_move = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::max(0, std::stoi(argv[++i]));
        } else if (arg == "--max-replay-nodes" && i + 1 < argc) {
            max_replay_nodes = std::max(0LL, std::stoll(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--jobs" && i + 1 < argc) {
            jobs = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--time") {
            time_mode = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--color") {
            color_output = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg.rfind("--", 0) == 0) {
            fmt::print(stderr, "Unknown or malformed option: {}\n", arg);
            return 1;
        } else {
            positional_args.push_back({i, arg});
        }
    }

    bool batch_uses_positional_engine = positional_args.size() > 2
                                     && !engine_path_explicit
                                     && canBePositionalEngine(positional_args[0].second)
                                     && containsLogTarget(positional_args, 1);

    if (positional_args.size() == 1) {
        logfile_arg_index = positional_args[0].first;
        logfile = positional_args[0].second;
    } else if (positional_args.size() == 2
            && !engine_path_explicit
            && isPositionalEngine(positional_args[0].second, positional_args[1].second)) {
        engine_path = positional_args[0].second;
        logfile_arg_index = positional_args[1].first;
        logfile = positional_args[1].second;
    } else if (batch_uses_positional_engine) {
        engine_path = positional_args[0].second;
    }

    bool batch_input = positional_args.size() > 1 && logfile.empty();
    std::vector<std::string> logfile_targets;
    std::vector<int> logfile_arg_indices;
    if (batch_input) {
        size_t first_target = batch_uses_positional_engine ? 1 : 0;
        for (size_t i = first_target; i < positional_args.size(); ++i) {
            const auto& [index, path] = positional_args[i];
            logfile_targets.push_back(path);
            logfile_arg_indices.push_back(index);
        }
    } else if (!logfile.empty()) {
        logfile_targets.push_back(logfile);
        logfile_arg_indices.push_back(logfile_arg_index);
    }

    if (logfile_targets.empty() && !isatty(STDIN_FILENO))
        logfile_targets.push_back("-");

    if (logfile_targets.empty()) {
        fmt::print(stderr, "ERROR: No logfile specified\n\n");
        print_help(argv[0]);
        return 1;
    }

    if (!analyze && analysis_target == "log") {
        fmt::print(stderr, "ERROR: --log cannot be used with --no-analysis\n");
        return 1;
    }

    bool cache_enabled = analyze
                      && start_move == 0
                      && count < 0;
    bool stdin_input = std::find(logfile_targets.begin(), logfile_targets.end(), "-") != logfile_targets.end();

    bool logfile_is_directory = !batch_input && std::filesystem::is_directory(logfile);
    bool run_as_batch = batch_input || logfile_is_directory || stdin_input;

    bool warn_log_time = analyze
                      && time_mode
                      && analysis_target == "log"
                      && std::getenv(kSuppressLogTimeWarning) == nullptr;
    if (warn_log_time) {
        fmt::print(stderr,
                   "WARNING: --time is ignored with --log.\n");
        if (run_as_batch)
            setenv(kSuppressLogTimeWarning, "1", 1);
    }

    if (run_as_batch)
        return runLogs(collectLogTargets(logfile_targets), argc, argv, logfile_arg_indices, jobs);

    if (std::filesystem::path(logfile).extension() != ".log") {
        fmt::print(stderr, "ERROR: replay needs a .log file.\n");
        return 1;
    }

    try {
        std::filesystem::path logfile_path = logfile;
        std::filesystem::path report_path;
        ParsedLog parsed = readLog(logfile, max_replay_nodes);
        std::vector<LogEntry> entries = parsed.entries;
        trimPostGameEntriesFromSiblingPgn(entries, logfile_path);
        if (entries.empty()) {
            fmt::print(stderr, "ERROR: No UCI go/bestmove pairs found in '{}'\n", logfile);
            return 1;
        }

        std::optional<AnalysisCache> cache;
        LogEntry final_log_entry = entries.back();
        std::vector<LogEntry> full_log_entries = entries;
        std::string full_log_timeout_report = formatTimeoutReport(entries, final_log_entry.fullmove);
        std::string full_log_game_report = formatGameStatusReport(entries, full_log_timeout_report);
        bool needs_candidate = !analyze || analysis_target != "log";
        if (cache_enabled) {
            if (needs_candidate && !executableExists(engine_path)) {
                fmt::print(stderr, "ERROR: Engine '{}' not found or not executable\n", engine_path);
                return 1;
            }
            if (!executableExists(reference_path)) {
                fmt::print(stderr, "ERROR: Reference engine '{}' not found or not executable\n", reference_path);
                fmt::print(stderr, "Use --reference <path> or --no-analysis.\n");
                return 1;
            }

            EngineConfig candidate_config = analysis_target == "log"
                ? EngineConfig{"unused", "none"}
                : probeEngineConfig(engine_path, effectiveSetoptions(parsed.setoptions, threads));
            auto reference_config = probeEngineConfig(reference_path, {});
            cache = buildAnalysisCache(logfile_path, candidate_config, reference_config,
                                       time_mode, max_replay_nodes,
                                       reference_limit, analysis_target);
            report_path = analysisPath(logfile_path, cache->key, analysis_target);

            if (!force && std::filesystem::exists(report_path)) {
                bool batch_mode = batchMode();
                std::string cached_report = readFile(report_path);
                std::string cached_body = cached_report;
                std::string cached_provenance;
                if (startsWith(cached_report, "analysis-key ")) {
                    size_t newline = cached_report.find('\n');
                    cached_provenance = cached_report.substr(0, newline);
                    cached_body = newline == std::string::npos ? "" : cached_report.substr(newline + 1);
                }
                if (cached_provenance == cache->provenance) {
                    std::string updated_body = replaceDerivedReportLines(cached_body,
                                                                         full_log_game_report,
                                                                         full_log_timeout_report);
                    if (updated_body != cached_body) {
                        cached_body = updated_body;
                        std::string updated_report = cached_provenance + "\n" + cached_body;
                        writeFile(report_path, updated_report);
                    }

                    if (!batch_mode)
                        fmt::print("cached: {}\n", report_path.string());
                    if (verbose)
                        fmt::print("{}\n", cache->provenance);
                    printBatchBlock(formatDiagnosticsReport(entries, final_log_entry.fullmove,
                                                            color_output, verbose));
                    printSummaryReport(cached_body, color_output, verbose);
                    if (cached_body.empty() || cached_body.back() != '\n')
                        fmt::print("\n");
                    return reportTriggersFailure(cached_body) ? 1 : 0;
                }
            }
        }

        int total_entries = (int)entries.size();
        int display_total = entries.back().fullmove;
        if (print_move_output)
            printBatchBlock(fmt::format("Extracted {} go commands and {} logged moves\n",
                                        total_entries, total_entries));
        if (verbose && cache)
            fmt::print("{}\n", cache->provenance);

        bool full_log_range = start_move <= 0 && count < 0;

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
            if (print_move_output) {
                printBatchBlock(fmt::format("Starting at fullmove {}; skipped {} log entries; {} remaining\n",
                                            entries.front().fullmove, skipped, entries.size()));
            }
        }

        if (count >= 0 && (int)entries.size() > count) {
            entries.erase(entries.begin() + count, entries.end());
            if (print_move_output)
                printBatchBlock(fmt::format("Limiting to {} moves\n", count));
        }

        ReplayLineWidths line_widths = replayLineWidths(entries, display_total);

        if (needs_candidate && !executableExists(engine_path)) {
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

        if (analyze && analysis_target == "log") {
            bool progress = isatty(STDOUT_FILENO) || batchMode();
            std::vector<AnalysisEntry> report;
            AnalysisStats stats;
            int analysis_failures = 0;

            bool analyzed_sequence = analyzeLoggedPositionSequence(*reference,
                                                                   full_log_entries,
                                                                   entries,
                                                                   full_log_range,
                                                                   reference_limit,
                                                                   display_total,
                                                                   progress,
                                                                   print_move_output,
                                                                   color_output,
                                                                   verbose,
                                                                   report,
                                                                   stats,
                                                                   analysis_failures);
            if (!analyzed_sequence) {
                analyzeLoggedMoves(*reference, entries, reference_limit, display_total,
                                   line_widths, progress, print_move_output,
                                   color_output, verbose, report, stats, analysis_failures);
            }

            bool includes_final_log_entry = !entries.empty() && sameLogEntry(entries.back(), final_log_entry);
            std::string timeout_report = includes_final_log_entry ? full_log_timeout_report : "";
            std::string game_report = includes_final_log_entry ? full_log_game_report : "";
            std::string analysis_report_body = formatAnalysisReport(report, stats, analysis_failures,
                                                                    false, true,
                                                                    game_report, timeout_report);
            std::string analysis_report = cache
                ? cache->provenance + "\n" + analysis_report_body
                : analysis_report_body;

            if (cache_enabled) {
                if (print_move_output)
                    fmt::print("\n");
                writeFile(report_path, analysis_report);
                printBatchBlock(fmt::format("Analysis saved     : {}\n", report_path.string()));
            }

            if (!print_move_output) {
                printBatchBlock(formatDiagnosticsReport(entries, display_total,
                                                        color_output, verbose));
            }
            printSummaryReport(analysis_report_body, color_output, verbose);
            if (analysis_report_body.empty() || analysis_report_body.back() != '\n')
                fmt::print("\n");

            return reportTriggersFailure(analysis_report_body) ? 1 : 0;
        }

        auto make_engine = [&] {
            auto engine = std::make_unique<EngineProcess>(engine_path, verbose);
            initializeEngine(*engine, parsed.setoptions, threads);
            return engine;
        };

        std::unique_ptr<EngineProcess> engine = make_engine();

        int searched = 0;
        int mismatches = 0;
        double min_wdl = 0.0;
        double max_wdl = 0.0;
        double final_wdl = 0.0;
        bool progress = isatty(STDOUT_FILENO) || batchMode();
        std::vector<AnalysisEntry> report;
        AnalysisStats stats;
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
                std::string analyzed_move = analysis_target == "log"
                    ? entry.expected
                    : result.bestmove;
                MoveValidation validation = validateMove(*reference,
                                                         entry.position,
                                                         analyzed_move,
                                                         reference_limit,
                                                         entry.depth,
                                                         entry.fullmove,
                                                         display_total,
                                                         progress);
                if (progress)
                    fmt::print("\r\033[K");

                reference_suffix = formatReferenceInline(validation, color_output);
                appendAnalysisEntry(report, stats, analysis_failures, validation,
                                    entry, analyzed_move,
                                    analysis_target == "replay" && mismatch ? entry.expected : "",
                                    display_total);
            }

            std::string replay_display = mismatch
                ? fmt::format("{} != log {}", played_display, expected_display)
                : played_display;
            if (print_move_output) {
                printBatchBlock(fmt::format("[{:>{}}/{}] {:<{}} | WDL {:+.2f} :: {}\n",
                                            entry.fullmove, line_widths.fullmove, display_total,
                                            replay_display, line_widths.replay,
                                            result.wdl, reference_suffix));
                for (const auto& diagnostic : entry.diagnostics)
                    printBatchBlock(formatDiagnosticLine(diagnostic, entry.fullmove, display_total,
                                                        color_output, verbose) + "\n");
                for (const auto& diagnostic : result.diagnostics)
                    printBatchBlock(formatDiagnosticLine(diagnostic, entry.fullmove, display_total,
                                                        color_output, verbose) + "\n");
                fflush(stdout);
            }

            if (mismatch)
                engine.reset();
        }

        std::string analysis_report_body;
        std::string game_report;
        std::string timeout_report;
        bool printed_analysis_save = false;
        if (analyze) {
            bool includes_final_log_entry = !entries.empty() && sameLogEntry(entries.back(), final_log_entry);
            timeout_report = includes_final_log_entry ? full_log_timeout_report : "";
            game_report = includes_final_log_entry ? full_log_game_report : "";
            analysis_report_body = formatAnalysisReport(report, stats, analysis_failures,
                                                        false, true,
                                                        game_report, timeout_report);
            std::string analysis_report = cache
                ? cache->provenance + "\n" + analysis_report_body
                : analysis_report_body;
            if (cache_enabled) {
                fmt::print("\n");
                writeFile(report_path, analysis_report);
                printBatchBlock(fmt::format("Analysis saved     : {}\n", report_path.string()));
                printed_analysis_save = true;
            }
        }

        printBatchBlock(fmt::format("{}=== Replay ===\n",
                                    printed_analysis_save || !print_move_output ? "" : "\n"));
        printBatchBlock(fmt::format("Positions replayed : {}\n", searched));
        printBatchBlock(fmt::format("Bestmove matches   : {}/{} ({} differed)\n",
                                    searched - mismatches, searched, mismatches));
        printBatchBlock(fmt::format("Final WDL          : {:+.2f}\n", final_wdl));
        printBatchBlock(fmt::format("WDL range          : [{:+.2f}, {:+.2f}]\n", min_wdl, max_wdl));

        if (analyze) {
            fmt::print("\n");
            printSummaryReport(formatAnalysisReport(report, stats, analysis_failures, color_output, verbose,
                                                    game_report, timeout_report),
                               color_output, verbose);
        }

        return reportTriggersFailure(analysis_report_body) ? 1 : 0;
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }

    return 0;
}
