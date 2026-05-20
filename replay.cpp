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
#include <memory>
#include <optional>
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

#include "engine_process.hpp"
#include "lichess_analysis.hpp"
#include "reference_limit.hpp"
#include "board.hpp"
#include "movegen.hpp"

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
constexpr long long kDefaultMaxReplayNodes = 300'000;
constexpr long long kDefaultFixedReplayNodes = 100'000;
constexpr int kDefaultFixedReplayMovetimeMs = 1000;
constexpr int kDefaultReplayReferenceNodes = 200'000;

struct SearchResult {
    std::string bestmove;
    std::vector<std::string> diagnostics;
    double wdl = 0.0;
    int mate_in = 0;
    long long nodes = 0;
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

enum class ReferenceScoringMode {
    ConsecutivePosition,
    RootSearchmoves
};

struct MoveValidation {
    bool ok = false;
    bool reference_best = false;
    bool consecutive_position_scores = true;
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
    std::string eval_before;
    std::string eval_after;
    std::string fen;
    int fullmove = 1;
    int display_total = 1;
    int cp_loss = 0;
};

struct AnalysisStats {
    int moves = 0;
    long long cp_loss = 0;
    double accuracy = 0.0;
    double harmonic_accuracy_denominator = 0.0;
    int side_sign = 0;
    std::vector<std::pair<int, Score>> ply_scores;
};

struct CsvRow {
    std::filesystem::path log_path;
    int fullmove = 1;
    int ply_before = 0;
    std::string side;
    std::string log_move;
    std::string candidate_move;
    std::string reference_move;
    std::string oracle_best;
    int candidate_loss = 0;
    int reference_loss = 0;
    int delta_loss = 0;
    std::string candidate_score;
    std::string reference_score;
    std::string best_score;
    std::string replay_go;
    long long logged_nodes = 0;
    long long candidate_nodes = 0;
    long long reference_nodes = 0;
    double replay_wdl = 0.0;
};

struct ComparisonValidation {
    bool ok = false;
    std::string error;
    std::string label;
    std::string oracle_best;
    std::string best_score;
    Score candidate_score;
    Score reference_score;
    int candidate_loss = 0;
    int reference_loss = 0;
    int delta_loss = 0;
    int ply_before = 0;
};

struct ComparisonEntry {
    std::string label;
    std::string candidate;
    std::string reference;
    std::string oracle_best;
    int candidate_loss = 0;
    int reference_loss = 0;
    int delta_loss = 0;
    int fullmove = 1;
    int display_total = 1;
};

struct ComparisonStats {
    int positions = 0;
    int candidate_better = 0;
    int reference_better = 0;
    int equal = 0;
    int failures = 0;
    long long delta_loss = 0;
    int best_gain = 0;
    int worst_regression = 0;
    std::vector<int> nonzero_diffs;
};

struct AnalysisWidths {
    size_t played = 4;
    size_t best = 4;
    size_t eval = 4;
    size_t loss = 4;
};

struct ComparisonWidths {
    size_t candidate = 9;
    size_t reference = 9;
    size_t oracle_best = 9;
    size_t delta = 4;
};

struct ReplayLineWidths {
    size_t fullmove = 1;
    size_t replay = 18;
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

std::optional<int> parsePositiveInt(std::string_view text) {
    if (text.empty())
        return std::nullopt;

    long long value = 0;
    for (char ch : text) {
        if (!std::isdigit((unsigned char)ch))
            return std::nullopt;
        value = value * 10 + (ch - '0');
        if (value > std::numeric_limits<int>::max())
            return std::nullopt;
    }

    if (value <= 0)
        return std::nullopt;
    return static_cast<int>(value);
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

std::string csvEscape(std::string_view text) {
    bool quote = text.find_first_of(",\"\n\r") != std::string_view::npos;
    if (!quote)
        return std::string{text};

    std::string escaped = "\"";
    for (char ch : text) {
        if (ch == '"')
            escaped += "\"\"";
        else
            escaped += ch;
    }
    escaped += "\"";
    return escaped;
}

void writeCsvHeader(std::ostream& output) {
    output << "log,fullmove,ply,side,log_move,"
              "candidate_move,reference_move,oracle_best,"
              "candidate_loss,reference_loss,diff,"
              "candidate_score,reference_score,best_score,"
              "candidate_nodes,reference_nodes,replay_go,logged_nodes,replay_wdl\n";
}

void writeCsvRow(std::ostream& output, const CsvRow& row) {
    output << csvEscape(row.log_path.string()) << ','
           << row.fullmove << ','
           << row.ply_before << ','
           << row.side << ','
           << csvEscape(row.log_move) << ','
           << csvEscape(row.candidate_move) << ','
           << csvEscape(row.reference_move) << ','
           << csvEscape(row.oracle_best) << ','
           << row.candidate_loss << ','
           << row.reference_loss << ','
           << row.delta_loss << ','
           << csvEscape(row.candidate_score) << ','
           << csvEscape(row.reference_score) << ','
           << csvEscape(row.best_score) << ','
           << row.candidate_nodes << ','
           << row.reference_nodes << ','
           << csvEscape(row.replay_go) << ','
           << row.logged_nodes << ','
           << fmt::format("{:.4f}", row.replay_wdl) << '\n';
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
                            long long max_replay_nodes,
                            long long fixed_replay_nodes,
                            int fixed_replay_movetime_ms) {
    if (fixed_replay_nodes > 0)
        return fmt::format("go nodes {}", fixed_replay_nodes);
    if (fixed_replay_movetime_ms > 0)
        return fmt::format("go movetime {}", fixed_replay_movetime_ms);
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

template <enyo::Color Us>
std::optional<enyo::Move> resolveUciMoveForSide(enyo::Board& board, std::string_view uci) {
    const Movelist moves = generate_legal_moves<Us, false, false>(board);
    for (auto move : moves) {
        if (fmt::format("{}", move) == uci)
            return move;
    }

    return std::nullopt;
}

std::optional<enyo::Move> resolveUciMove(const enyo::Board& board, std::string_view uci) {
    enyo::Board copy = board;
    if (copy.side == enyo::white)
        return resolveUciMoveForSide<enyo::white>(copy, uci);
    return resolveUciMoveForSide<enyo::black>(copy, uci);
}

bool applyUciMove(enyo::Board& board, const std::string& move) {
    auto resolved = resolveUciMove(board, move);
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

std::string formatMoveDisplay(const std::string&, const std::string& move) {
    return move;
}

ReplayLineWidths replayLineWidths(const std::vector<LogEntry>& entries, int display_total) {
    ReplayLineWidths widths;
    widths.fullmove = std::to_string(std::max(1, display_total)).size();
    for (const auto& entry : entries) {
        std::string expected = formatMoveDisplay(entry.position, entry.expected);
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

std::string analysisEvalText(const AnalysisEntry& entry) {
    if (entry.eval_before.empty() || entry.eval_after.empty())
        return "";
    return fmt::format("{} -> {}", entry.eval_before, entry.eval_after);
}

AnalysisWidths analysisWidths(const std::vector<AnalysisEntry>& entries) {
    AnalysisWidths widths;
    for (const auto& entry : entries) {
        widths.played = std::max(widths.played, analysisPlayedText(entry).size());
        widths.best = std::max(widths.best, entry.best.size());
        widths.eval = std::max(widths.eval, analysisEvalText(entry).size());
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
    std::string line = fmt::format("{} {:<{}}  best: {:<{}}  eval: {:<{}}  loss: {:>{}}",
                                   label, analysisPlayedText(entry), widths.played,
                                   entry.best, widths.best,
                                   analysisEvalText(entry), widths.eval,
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
        return fmt::format("oracle {}{}",
                           colorizeJudgementToken(fmt::format("{:<{}}", judgement, judgement_width),
                                                  validation.label, color),
                           best_report);
    }

    std::string judgement = validation.reference_best
        ? (validation.best_score.empty() ? "best" : fmt::format("best {}", validation.best_score))
        : (validation.cp_loss == 0 ? "equal" : fmt::format("loss {}cp", validation.cp_loss));
    std::string label = validation.reference_best || validation.cp_loss == 0 ? "best" : "";

    return fmt::format("oracle {}{}",
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

void clearSearchProgress() {
    if (!batchMode())
        fmt::print("\r\033[K");
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
    std::string played_display = formatMoveDisplay(entry.position, entry.expected);
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

std::string formatAccuracyReport(const AnalysisStats& stats) {
    if (stats.moves == 0)
        return "";

    int accuracy = lichessGameAccuracy(stats.side_sign, stats.ply_scores)
        .value_or(std::clamp(static_cast<int>(std::lround(stats.accuracy / stats.moves)), 0, 100));
    int avg_loss = static_cast<int>(std::lround(static_cast<double>(stats.cp_loss) / stats.moves));
    return fmt::format("{:<11} {}%\n{:<11} {}cp\n",
                       "accuracy:", accuracy,
                       "avg loss:", avg_loss);
}

std::string formatGameScoreReport(const AnalysisStats& stats) {
    if (stats.moves == 0)
        return "";

    double mean_accuracy = stats.accuracy / stats.moves;
    double harmonic_accuracy = stats.harmonic_accuracy_denominator > 0.0
        ? static_cast<double>(stats.moves) / stats.harmonic_accuracy_denominator
        : mean_accuracy;
    int score = std::clamp(static_cast<int>(std::lround((mean_accuracy + harmonic_accuracy) / 2.0)),
                           0, 100);
    return fmt::format("{:<11} {}\n", "score:", score);
}

std::string formatReplaySummaryReport(int searched,
                                      int mismatches,
                                      double final_wdl,
                                      double min_wdl,
                                      double max_wdl) {
    return fmt::format("positions:  {}\n"
                       "matches:    {}/{} ({} differed)\n"
                       "final wdl:  {:+.2f}\n"
                       "wdl range:  [{:+.2f}, {:+.2f}]\n",
                       searched,
                       searched - mismatches,
                       searched,
                       mismatches,
                       final_wdl,
                       min_wdl,
                       max_wdl);
}

std::string formatSignedCp(long long value) {
    return fmt::format("{:+}cp", value);
}

std::string comparisonMoveText(const ComparisonEntry& entry) {
    size_t fullmove_width = std::to_string(std::max(1, entry.display_total)).size();
    return fmt::format("[{:>{}}/{}] {}",
                       entry.fullmove, fullmove_width,
                       entry.display_total, entry.candidate);
}

ComparisonWidths comparisonWidths(const std::vector<ComparisonEntry>& entries) {
    ComparisonWidths widths;
    for (const auto& entry : entries) {
        widths.candidate = std::max(widths.candidate, comparisonMoveText(entry).size());
        widths.reference = std::max(widths.reference, entry.reference.size());
        widths.oracle_best = std::max(widths.oracle_best, entry.oracle_best.size());
        widths.delta = std::max(widths.delta, formatSignedCp(entry.delta_loss).size());
    }
    return widths;
}

std::string formatComparisonEntry(const ComparisonEntry& entry,
                                  const ComparisonWidths& widths) {
    return fmt::format("{:<17} {:<{}}  reference: {:<{}}  oracle: {:<{}}  loss: {}/{}  diff: {:>{}}",
                       entry.label + ":",
                       comparisonMoveText(entry), widths.candidate,
                       entry.reference, widths.reference,
                       entry.oracle_best, widths.oracle_best,
                       entry.candidate_loss, entry.reference_loss,
                       formatSignedCp(entry.delta_loss), widths.delta);
}

int medianComparisonDiff(std::vector<int> diffs) {
    if (diffs.empty())
        return 0;

    std::sort(diffs.begin(), diffs.end());
    size_t middle = diffs.size() / 2;
    if (diffs.size() % 2 != 0)
        return diffs[middle];

    return static_cast<int>(std::lround((diffs[middle - 1] + diffs[middle]) / 2.0));
}

std::string formatComparisonReport(const std::vector<ComparisonEntry>& report,
                                   const ComparisonStats& stats) {
    std::string output;
    if (report.empty())
        output += "No candidate/reference differences found.\n";
    else {
        ComparisonWidths widths = comparisonWidths(report);
        for (const auto& entry : report)
            output += formatComparisonEntry(entry, widths) + "\n";
    }

    if (stats.failures > 0)
        output += fmt::format("Analysis failures  : {}\n", stats.failures);

    output += fmt::format("positions:         {}\n"
                          "candidate better:  {}\n"
                          "reference better:  {}\n"
                          "equal:             {}\n"
                          "diff:              {}\n"
                          "median diff:       {}\n"
                          "worst regression:  {}\n"
                          "best gain:         {}\n",
                          stats.positions,
                          stats.candidate_better,
                          stats.reference_better,
                          stats.equal,
                          formatSignedCp(static_cast<int>(stats.delta_loss)),
                          formatSignedCp(medianComparisonDiff(stats.nonzero_diffs)),
                          formatSignedCp(stats.worst_regression),
                          formatSignedCp(stats.best_gain));
    return output;
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
    output += formatGameScoreReport(stats);

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

std::filesystem::path tempCsvPath(size_t index) {
    auto directory = std::filesystem::temp_directory_path();
    return directory / fmt::format("replay-job-{}-{}.csv", getpid(), index);
}

std::vector<std::string> effectiveSetoptions(const std::vector<std::string>& log_setoptions,
                                             int threads) {
    std::vector<std::string> setoptions = log_setoptions;
    if (threads > 0)
        setoptions.push_back(fmt::format("setoption name Threads value {}", threads));
    return setoptions;
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
            printSearchProgress(fmt::format("{} nodes {}/{}",
                                            progress_text,
                                            formatNodeCount(0),
                                            formatNodeCount(target)));
        else
            printSearchProgress(fmt::format("{} depth 0/{}", progress_text, target));
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
            throw std::runtime_error("analysis engine stopped during search");

        updateReferenceScore(result, *line, stm_sign);
        if (progress && line->find("info depth ") != std::string::npos) {
            if (limit.kind == ReferenceLimitKind::Nodes) {
                long long current_nodes = parseLongField(*line, "nodes");
                if (current_nodes > 0 && current_nodes != last_reported_nodes) {
                    last_reported_nodes = current_nodes;
                    printSearchProgress(fmt::format("{} nodes {}/{}",
                                                    progress_text,
                                                    formatNodeCount(std::min(current_nodes,
                                                                             static_cast<long long>(target))),
                                                    formatNodeCount(target)));
                }
            } else {
                int current_depth = extractDepth(*line);
                if (current_depth > 0 && current_depth != last_reported_depth) {
                    last_reported_depth = current_depth;
                    printSearchProgress(fmt::format("{} depth {}/{}",
                                                    progress_text,
                                                    current_depth,
                                                    target));
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
                            bool reset_reference = true,
                            ReferenceScoringMode scoring_mode = ReferenceScoringMode::ConsecutivePosition) {
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
    validation.best_display = formatMoveDisplay(position, best.bestmove);
    validation.best_score = formatScore(best_score);
    validation.ply_before = plyBeforeMove(position, fullmove);
    validation.ply_after = validation.ply_before + 1;

    validation.reference_best = best.bestmove == played_move;
    validation.consecutive_position_scores = scoring_mode == ReferenceScoringMode::ConsecutivePosition;

    ReferenceResult played;
    if (scoring_mode == ReferenceScoringMode::RootSearchmoves) {
        if (validation.reference_best) {
            played = best;
        } else {
            played = referenceSearch(reference,
                                     position,
                                     resolved_limit,
                                     progress,
                                     fmt::format("{} [{}/{}] played-move",
                                                 progress_verb, fullmove, display_total),
                                     played_move,
                                     true);
        }
    } else {
        auto played_position = positionAfterMove(position, played_move);
        if (!played_position) {
            validation.error = "could not apply played move";
            return validation;
        }

        played = referenceSearch(reference,
                                 *played_position,
                                 resolved_limit,
                                 progress,
                                 fmt::format("{} [{}/{}] played-position",
                                             progress_verb, fullmove, display_total),
                                 "",
                                 false);
    }
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
    if (validation.reference_best) {
        validation.after_score_white = validation.before_score_white;
        validation.cp_loss = 0;
        validation.accuracy = 100.0;
        validation.label.clear();
    } else if (validation.cp_loss == 0 && isReportableJudgement(validation.label)) {
        validation.label.clear();
    }
    return validation;
}

std::string comparisonLabel(int delta_loss) {
    if (delta_loss > 0)
        return "candidate better";
    if (delta_loss < 0)
        return "candidate worse";
    return "equal";
}

ReferenceResult evaluateOracleMove(EngineProcess& oracle,
                                   const std::string& position,
                                   const ReferenceLimit& limit,
                                   bool progress,
                                   const std::string& progress_text,
                                   const std::string& move,
                                   const ReferenceResult& best) {
    if (move == best.bestmove)
        return best;
    return referenceSearch(oracle, position, limit, progress, progress_text, move, true);
}

ComparisonValidation validateComparison(EngineProcess& oracle,
                                        const std::string& position,
                                        const std::string& candidate_move,
                                        const std::string& reference_move,
                                        const ReferenceLimit& limit,
                                        int logged_depth,
                                        int fullmove,
                                        int display_total,
                                        bool progress) {
    ComparisonValidation validation;
    if (candidate_move.empty() || candidate_move == "(none)") {
        validation.error = "no candidate move";
        return validation;
    }
    if (reference_move.empty() || reference_move == "(none)") {
        validation.error = "no reference move";
        return validation;
    }

    ReferenceLimit resolved_limit = resolveReferenceLimit(limit, logged_depth);
    ReferenceResult best = referenceSearch(oracle,
                                          position,
                                          resolved_limit,
                                          progress,
                                          fmt::format("analyzing [{}/{}] oracle-best",
                                                      fullmove, display_total),
                                          "",
                                          true);
    if (!best.has_score) {
        validation.error = "oracle returned no score";
        return validation;
    }

    ReferenceResult candidate = evaluateOracleMove(oracle,
                                                   position,
                                                   resolved_limit,
                                                   progress,
                                                   fmt::format("analyzing [{}/{}] candidate-move",
                                                               fullmove, display_total),
                                                   candidate_move,
                                                   best);
    if (!candidate.has_score) {
        validation.error = "oracle returned no candidate score";
        return validation;
    }

    ReferenceResult reference = candidate_move == reference_move
        ? candidate
        : evaluateOracleMove(oracle,
                             position,
                             resolved_limit,
                             progress,
                             fmt::format("analyzing [{}/{}] reference-move",
                                         fullmove, display_total),
                             reference_move,
                             best);
    if (!reference.has_score) {
        validation.error = "oracle returned no reference score";
        return validation;
    }

    int mover_sign = sideToMoveSign(position);
    validation.ok = true;
    validation.oracle_best = best.bestmove;
    validation.best_score = formatScore(scoreForSide(best.score_white, mover_sign));
    validation.candidate_score = scoreForSide(candidate.score_white, mover_sign);
    validation.reference_score = scoreForSide(reference.score_white, mover_sign);
    validation.candidate_loss = candidate_move == best.bestmove
        ? 0
        : lichessCpLoss(best.score_white, candidate.score_white, mover_sign);
    validation.reference_loss = reference_move == best.bestmove
        ? 0
        : lichessCpLoss(best.score_white, reference.score_white, mover_sign);
    validation.delta_loss = validation.reference_loss - validation.candidate_loss;
    validation.label = comparisonLabel(validation.delta_loss);
    validation.ply_before = plyBeforeMove(position, fullmove);
    return validation;
}

ReferenceLimit sequenceAnalysisLimit(const ReferenceLimit& limit) {
    if (limit.kind == ReferenceLimitKind::LoggedDepth)
        return ReferenceLimit{};
    return limit;
}

std::unordered_map<int, PositionEvaluation> analyzeFishnetPositionSequence(EngineProcess& reference,
                                                                          const PositionSequence& sequence,
                                                                          const ReferenceLimit& reference_limit,
                                                                          bool progress) {
    std::unordered_map<int, PositionEvaluation> evaluations;
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
        clearSearchProgress();

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
    validation.best_display = formatMoveDisplay(entry.position, validation.bestmove);
    validation.best_score = formatScore(best_score);
    validation.cp_loss = lichessCpLoss(validation.before_score_white,
                                       validation.after_score_white,
                                       mover_sign);
    validation.accuracy = lichessAccuracy(best_score, played_score);
    validation.label = lichessJudgement(best_score, played_score);
    if (validation.reference_best) {
        validation.after_score_white = validation.before_score_white;
        validation.cp_loss = 0;
        validation.accuracy = 100.0;
        validation.label.clear();
    } else if (validation.cp_loss == 0 && isReportableJudgement(validation.label)) {
        validation.label.clear();
    }
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
    stats.harmonic_accuracy_denominator += 1.0 / std::max(1.0, validation.accuracy);
    if (stats.side_sign == 0)
        stats.side_sign = sideToMoveSign(entry.position);
    if (validation.consecutive_position_scores && validation.ply_before > 0)
        stats.ply_scores.push_back({validation.ply_before, validation.before_score_white});
    if (validation.consecutive_position_scores && validation.ply_after > 0)
        stats.ply_scores.push_back({validation.ply_after, validation.after_score_white});

    if (!isReportableJudgement(validation.label))
        return;

    report.push_back({
        validation.label,
        analyzed_move,
        expected_move,
        validation.bestmove,
        formatScore(scoreForSide(validation.before_score_white, sideToMoveSign(entry.position))),
        formatScore(scoreForSide(validation.after_score_white, sideToMoveSign(entry.position))),
        entry.fen,
        entry.fullmove,
        display_total,
        validation.cp_loss
    });
}

void appendComparisonEntry(std::vector<ComparisonEntry>& report,
                           ComparisonStats& stats,
                           const ComparisonValidation& validation,
                           const LogEntry& entry,
                           const std::string& candidate_move,
                           const std::string& reference_move,
                           int display_total) {
    if (!validation.ok) {
        stats.failures++;
        return;
    }

    stats.positions++;
    stats.delta_loss += validation.delta_loss;
    if (validation.delta_loss > 0)
        stats.candidate_better++;
    else if (validation.delta_loss < 0)
        stats.reference_better++;
    else
        stats.equal++;

    if (validation.delta_loss == 0)
        return;

    stats.nonzero_diffs.push_back(validation.delta_loss);
    stats.best_gain = std::max(stats.best_gain, validation.delta_loss);
    stats.worst_regression = std::min(stats.worst_regression, validation.delta_loss);

    report.push_back({
        validation.label,
        candidate_move,
        reference_move,
        validation.oracle_best,
        validation.candidate_loss,
        validation.reference_loss,
        validation.delta_loss,
        entry.fullmove,
        display_total
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
            clearSearchProgress();
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
    long long last_nodes = 0;
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
            long long nodes = parseLongField(*line, "nodes");
            if (nodes > 0)
                last_nodes = nodes;
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
                clearSearchProgress();
            return {extractMove(*line), diagnostics, wdl, mate_in, last_nodes};
        }
    }

    throw std::runtime_error("engine stopped before bestmove");
}

ParsedLog readLog(const std::filesystem::path& logfile,
                  long long max_replay_nodes,
                  long long fixed_replay_nodes,
                  int fixed_replay_movetime_ms) {
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
                                                max_replay_nodes,
                                                fixed_replay_nodes,
                                                fixed_replay_movetime_ms);
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
                         const std::filesystem::path& output_path,
                         const std::filesystem::path& csv_output_path) {
    std::string command = shellQuote(argv[0]);
    bool inserted_log = false;
    for (int arg = 1; arg < argc; ++arg) {
        if (std::string(argv[arg]) == "--jobs" && arg + 1 < argc) {
            arg++;
            continue;
        }
        if (std::string(argv[arg]) == "--csv")
            continue;

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

    if (!csv_output_path.empty())
        command += " --csv";

    if (!csv_output_path.empty()) {
        command += " > " + shellQuote(csv_output_path.string());
        command += " 2> " + shellQuote(output_path.string());
    } else {
        command += " > " + shellQuote(output_path.string()) + " 2>&1";
    }
    return command;
}

struct RunningJob {
    pid_t pid = -1;
    size_t index = 0;
    std::filesystem::path output_path;
    std::filesystem::path csv_output_path;
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

void printJobOutput(const std::filesystem::path& path, bool to_stderr) {
    std::ifstream file(path);
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line)) {
        if (startsWith(line, kBatchProgressPrefix))
            continue;
        fmt::print(to_stderr ? stderr : stdout, "{}\n", line);
    }
}

void appendCsvOutput(const std::filesystem::path& source,
                     bool& wrote_header,
                     std::ostream& output) {
    if (source.empty())
        return;

    std::ifstream input(source);
    if (!input.is_open())
        return;

    std::string line;
    bool first_line = true;
    while (std::getline(input, line)) {
        if (first_line) {
            first_line = false;
            if (wrote_header)
                continue;
            wrote_header = true;
        }
        output << line << '\n';
    }
}

struct BatchComparisonLogSummary {
    int positions = 0;
    int candidate_better = 0;
    int reference_better = 0;
    int equal = 0;
    int diff = 0;
    int worst_regression = 0;
    int best_gain = 0;
};

struct BatchComparisonSummary {
    int logs = 0;
    int candidate_logs = 0;
    int reference_logs = 0;
    int equal_logs = 0;
    long long positions = 0;
    long long candidate_better = 0;
    long long reference_better = 0;
    long long equal = 0;
    long long diff = 0;
    int worst_regression = 0;
    int best_gain = 0;
    bool has_worst_regression = false;
    bool has_best_gain = false;
};

std::optional<int> parsePlainIntLine(const std::string& line,
                                     const std::string& label) {
    if (!startsWith(line, label))
        return std::nullopt;

    try {
        return std::stoi(trim(line.substr(label.size())));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> parseSignedCpLine(const std::string& line,
                                     const std::string& label) {
    if (!startsWith(line, label))
        return std::nullopt;

    std::string value = trim(line.substr(label.size()));
    if (value.ends_with("cp"))
        value.resize(value.size() - 2);

    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<BatchComparisonLogSummary> parseComparisonSummary(
    const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return std::nullopt;

    BatchComparisonLogSummary summary;
    bool saw_positions = false;
    bool saw_candidate = false;
    bool saw_reference = false;
    bool saw_equal = false;
    bool saw_diff = false;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (auto value = parsePlainIntLine(line, "positions:")) {
            summary.positions = *value;
            saw_positions = true;
        } else if (auto value = parsePlainIntLine(line, "candidate better:")) {
            summary.candidate_better = *value;
            saw_candidate = true;
        } else if (auto value = parsePlainIntLine(line, "reference better:")) {
            summary.reference_better = *value;
            saw_reference = true;
        } else if (auto value = parsePlainIntLine(line, "equal:")) {
            summary.equal = *value;
            saw_equal = true;
        } else if (auto value = parseSignedCpLine(line, "diff:")) {
            summary.diff = *value;
            saw_diff = true;
        } else if (auto value = parseSignedCpLine(line, "worst regression:")) {
            summary.worst_regression = *value;
        } else if (auto value = parseSignedCpLine(line, "best gain:")) {
            summary.best_gain = *value;
        }
    }

    if (!saw_positions || !saw_candidate || !saw_reference || !saw_equal || !saw_diff)
        return std::nullopt;
    return summary;
}

void appendBatchComparisonSummary(BatchComparisonSummary& batch,
                                  const BatchComparisonLogSummary& log) {
    batch.logs++;
    if (log.diff > 0)
        batch.candidate_logs++;
    else if (log.diff < 0)
        batch.reference_logs++;
    else
        batch.equal_logs++;

    batch.positions += log.positions;
    batch.candidate_better += log.candidate_better;
    batch.reference_better += log.reference_better;
    batch.equal += log.equal;
    batch.diff += log.diff;

    if (!batch.has_worst_regression || log.worst_regression < batch.worst_regression) {
        batch.worst_regression = log.worst_regression;
        batch.has_worst_regression = true;
    }
    if (!batch.has_best_gain || log.best_gain > batch.best_gain) {
        batch.best_gain = log.best_gain;
        batch.has_best_gain = true;
    }
}

std::string formatBatchComparisonSummary(const BatchComparisonSummary& summary) {
    if (summary.logs == 0)
        return "";

    int avg_log = static_cast<int>(std::lround(static_cast<double>(summary.diff)
                                             / summary.logs));
    int avg_pos = summary.positions == 0
        ? 0
        : static_cast<int>(std::lround(static_cast<double>(summary.diff)
                                     / static_cast<double>(summary.positions)));

    return fmt::format("\nBatch comparison:\n"
                       "logs:              {}\n"
                       "positions:         {}\n"
                       "candidate better:  {} logs, {} moves\n"
                       "reference better:  {} logs, {} moves\n"
                       "equal:             {} logs, {} moves\n"
                       "total diff:        {}\n"
                       "avg diff/log:      {}\n"
                       "avg diff/pos:      {}\n"
                       "worst regression:  {}\n"
                       "best gain:         {}\n",
                       summary.logs,
                       summary.positions,
                       summary.candidate_logs, summary.candidate_better,
                       summary.reference_logs, summary.reference_better,
                       summary.equal_logs, summary.equal,
                       formatSignedCp(summary.diff),
                       formatSignedCp(avg_log),
                       formatSignedCp(avg_pos),
                       formatSignedCp(summary.worst_regression),
                       formatSignedCp(summary.best_gain));
}

int waitForBatchJob(const RunningJob& job,
                    const std::vector<std::filesystem::path>& logs,
                    size_t completed,
                    bool progress_to_stderr = false) {
    (void)logs;
    (void)completed;
    (void)progress_to_stderr;
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
            int jobs,
            bool csv_output,
            bool aggregate_comparison) {
    if (logs.empty()) {
        fmt::print(stderr, "ERROR: No .log files found\n");
        return 1;
    }
    if (logs.size() < 2)
        jobs = 1;

    int failures = 0;
    bool wrote_csv_header = false;
    BatchComparisonSummary comparison_summary;
    setenv(kReplayBatch, "1", 1);
    if (jobs <= 1) {
        for (size_t i = 0; i < logs.size(); ++i) {
            fmt::print(csv_output ? stderr : stdout,
                       "\n[{}/{}] {}\n", i + 1, logs.size(), logs[i].filename().string());
            std::fflush(csv_output ? stderr : stdout);

            auto output_path = tempOutputPath(i);
            auto csv_output_path = csv_output ? tempCsvPath(i) : std::filesystem::path{};
            std::string command = batchCommand(argv, argc, logfile_arg_indices,
                                               logs[i], output_path, csv_output_path);
            pid_t pid = startBatchJob(command);
            if (pid < 0)
                throw std::runtime_error(fmt::format("failed to start job: {}", strerror(errno)));
            int rc = waitForBatchJob({pid, i, output_path, csv_output_path,
                                      std::chrono::steady_clock::now()},
                                     logs, i, csv_output);
            printJobOutput(output_path, csv_output);
            if (aggregate_comparison) {
                if (auto summary = parseComparisonSummary(output_path))
                    appendBatchComparisonSummary(comparison_summary, *summary);
            }
            appendCsvOutput(csv_output_path, wrote_csv_header, std::cout);
            std::filesystem::remove(output_path);
            if (!csv_output_path.empty())
                std::filesystem::remove(csv_output_path);
            if (rc == 128 + SIGINT || interruptedStatus(rc))
                return 128 + SIGINT;
            if (rc != 0)
                failures++;
        }
        if (aggregate_comparison)
            fmt::print(csv_output ? stderr : stdout, "{}",
                       formatBatchComparisonSummary(comparison_summary));
        return failures == 0 ? 0 : 1;
    }

    std::vector<RunningJob> running;
    std::vector<std::filesystem::path> output_paths(logs.size());
    std::vector<std::filesystem::path> csv_output_paths(logs.size());
    size_t next = 0;
    size_t completed = 0;
    fmt::print(csv_output ? stderr : stdout,
               "Running {} logs with {} jobs; output prints as each log finishes.\n",
               logs.size(), jobs);
    std::fflush(csv_output ? stderr : stdout);

    auto launch_next = [&] {
        auto output_path = tempOutputPath(next);
        auto csv_output_path = csv_output ? tempCsvPath(next) : std::filesystem::path{};
        std::string command = batchCommand(argv, argc, logfile_arg_indices,
                                           logs[next], output_path, csv_output_path);
        pid_t pid = startBatchJob(command);
        if (pid < 0)
            throw std::runtime_error(fmt::format("failed to start job: {}", strerror(errno)));
        output_paths[next] = output_path;
        csv_output_paths[next] = csv_output_path;
        running.push_back({pid, next, output_path, csv_output_path,
                           std::chrono::steady_clock::now()});
        if (completed == 0) {
            fmt::print(csv_output ? stderr : stdout,
                       "analyzing [{}/{}] {}\n", next + 1, logs.size(), logs[next].filename().string());
            std::fflush(csv_output ? stderr : stdout);
        }
        next++;
    };
    auto print_finished = [&](const RunningJob& job, int status) {
        fmt::print(csv_output ? stderr : stdout,
                   "\n[{}/{}] {}\n",
                   job.index + 1, logs.size(), logs[job.index].filename().string());
        printJobOutput(job.output_path, csv_output);
        if (aggregate_comparison) {
            if (auto summary = parseComparisonSummary(job.output_path))
                appendBatchComparisonSummary(comparison_summary, *summary);
        }
        appendCsvOutput(job.csv_output_path, wrote_csv_header, std::cout);
        std::filesystem::remove(job.output_path);
        if (!job.csv_output_path.empty())
            std::filesystem::remove(job.csv_output_path);
        std::fflush(csv_output ? stderr : stdout);

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
            }
        }
    } catch (...) {
        terminateJobs(running);
        for (const auto& output_path : output_paths) {
            if (!output_path.empty())
                std::filesystem::remove(output_path);
        }
        for (const auto& csv_output_path : csv_output_paths) {
            if (!csv_output_path.empty())
                std::filesystem::remove(csv_output_path);
        }
        throw;
    }

    if (aggregate_comparison)
        fmt::print(csv_output ? stderr : stdout, "{}",
                   formatBatchComparisonSummary(comparison_summary));

    return failures == 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    std::string candidate_path = "enyo";
    std::string reference_path;
    std::string oracle_path = "stockfish";
    std::string logfile;
    int logfile_arg_index = -1;
    int start_move = 0;
    int count = -1;
    int threads = 1;
    int jobs = 1;
    long long max_replay_nodes = kDefaultMaxReplayNodes;
    long long fixed_replay_nodes = kDefaultFixedReplayNodes;
    int fixed_replay_movetime_ms = 0;
    ReferenceLimit oracle_limit{ReferenceLimitKind::Nodes, kDefaultReplayReferenceNodes};
    std::string analysis_target = "replay";
    bool csv_output = false;
    bool time_mode = false;
    bool analyze = true;
    bool verbose = false;
    bool color_output = false;
    bool print_move_output = false;
    bool candidate_path_explicit = false;
    bool reference_path_explicit = false;
    std::vector<std::pair<int, std::string>> positional_args;

    auto print_help = [&](const char* prog) {
        fmt::print(
            "Usage: {} [options] <logfile-or-directory> [more logs...]\n"
            "\n"
            "Replay UCI log searches and compare candidate output with a reference engine.\n"
            "Candidate replay uses fixed 100k nodes unless another replay budget is set.\n"
            "Multiple log paths are treated as batch input; non-.log paths are ignored.\n"
            "Use '-' or pipe newline-separated paths on stdin to read log targets from stdin.\n"
            "At the end, judge moves with an oracle engine.\n"
            "\n"
            "Options:\n"
            "  --candidate <path>  Candidate engine to replay with (default: enyo)\n"
            "  --reference <path>  Baseline engine to compare against\n"
            "  --oracle <path>     Oracle engine for judging moves (default: stockfish)\n"
            "  --oracle-nodes N    Oracle analysis nodes (default: 200000)\n"
            "  --oracle-depth N    Oracle analysis depth instead of nodes; 0 follows logged depth\n"
            "  --log               Analyze logged moves instead of replayed moves;\n"
            "                      does not run the candidate engine\n"
            "  --no-analysis       Replay only; do not run oracle analysis\n"
            "  --moves             Print per-position replay lines\n"
            "  --move N            Start at fullmove N\n"
            "  --count N           Replay at most N logged engine moves\n"
            "  --max-replay-nodes N\n"
            "                      Use logged nodes capped at N; 0 disables the cap\n"
            "  --fixed-nodes N     Replay candidate with exactly N nodes per position (default: 100000)\n"
            "  --fixed-movetime [MS]\n"
            "                      Replay candidate with fixed movetime; default 1000 ms\n"
            "  --csv               Write per-position replay analysis rows to stdout\n"
            "  --time              Replay with the original logged go wtime/btime command;\n"
            "                      ignored with --log and overrides logged-node replay\n"
            "  --threads N         Send `setoption name Threads value N` (default: 1)\n"
            "  --jobs N            Run up to N logs in parallel in batch mode (default: 1)\n"
            "  --color             Color judgement output\n"
            "  --verbose, -v       Print full UCI traffic and FENs\n"
            "  --help, -h          Show this help and exit\n",
            prog);
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "--candidate" && i + 1 < argc) {
            candidate_path = argv[++i];
            candidate_path_explicit = true;
        } else if (arg == "--reference" && i + 1 < argc) {
            reference_path = argv[++i];
            reference_path_explicit = true;
        } else if (arg == "--oracle" && i + 1 < argc) {
            oracle_path = argv[++i];
        } else if (arg == "--oracle-nodes" && i + 1 < argc) {
            oracle_limit = {ReferenceLimitKind::Nodes, std::max(1, std::stoi(argv[++i]))};
        } else if (arg == "--oracle-depth" && i + 1 < argc) {
            int depth = std::max(0, std::stoi(argv[++i]));
            oracle_limit = depth == 0
                ? ReferenceLimit{ReferenceLimitKind::LoggedDepth, 0}
                : ReferenceLimit{ReferenceLimitKind::Depth, depth};
        } else if (arg == "--log") {
            analysis_target = "log";
        } else if (arg == "--no-analysis") {
            analyze = false;
        } else if (arg == "--moves") {
            print_move_output = true;
        } else if (arg == "--move" && i + 1 < argc) {
            start_move = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::max(0, std::stoi(argv[++i]));
        } else if (arg == "--max-replay-nodes" && i + 1 < argc) {
            max_replay_nodes = std::max(0LL, std::stoll(argv[++i]));
            fixed_replay_nodes = 0;
            fixed_replay_movetime_ms = 0;
        } else if (arg == "--fixed-nodes" && i + 1 < argc) {
            fixed_replay_nodes = std::max(1LL, std::stoll(argv[++i]));
            fixed_replay_movetime_ms = 0;
        } else if (startsWith(arg, "--fixed-movetime=")) {
            auto value = parsePositiveInt(arg.substr(17));
            if (!value) {
                fmt::print(stderr, "Invalid --fixed-movetime value: {}\n", arg.substr(17));
                return 1;
            }
            fixed_replay_movetime_ms = *value;
            fixed_replay_nodes = 0;
        } else if (arg == "--fixed-movetime") {
            fixed_replay_movetime_ms = kDefaultFixedReplayMovetimeMs;
            fixed_replay_nodes = 0;
            if (i + 1 < argc) {
                auto value = parsePositiveInt(argv[i + 1]);
                if (value) {
                    fixed_replay_movetime_ms = *value;
                    i++;
                }
            }
        } else if (arg == "--csv") {
            csv_output = true;
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--jobs" && i + 1 < argc) {
            jobs = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--time") {
            time_mode = true;
            fixed_replay_nodes = 0;
            fixed_replay_movetime_ms = 0;
        } else if (arg == "--color") {
            color_output = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
            print_move_output = true;
        } else if (startsWith(arg, "-")) {
            fmt::print(stderr, "Unknown or malformed option: {}\n", arg);
            return 1;
        } else {
            positional_args.push_back({i, arg});
        }
    }

    bool batch_uses_positional_engine = positional_args.size() > 2
                                     && !candidate_path_explicit
                                     && canBePositionalEngine(positional_args[0].second)
                                     && containsLogTarget(positional_args, 1);

    if (positional_args.size() == 1) {
        logfile_arg_index = positional_args[0].first;
        logfile = positional_args[0].second;
    } else if (positional_args.size() == 2
            && !candidate_path_explicit
            && isPositionalEngine(positional_args[0].second, positional_args[1].second)) {
        candidate_path = positional_args[0].second;
        logfile_arg_index = positional_args[1].first;
        logfile = positional_args[1].second;
    } else if (batch_uses_positional_engine) {
        candidate_path = positional_args[0].second;
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
    if (reference_path_explicit && analysis_target == "log") {
        fmt::print(stderr, "ERROR: --reference compares replayed candidate output; it cannot be used with --log\n");
        return 1;
    }
    if (reference_path_explicit && !analyze) {
        fmt::print(stderr, "ERROR: --reference needs oracle analysis; remove --no-analysis\n");
        return 1;
    }
    if (csv_output && analysis_target == "log") {
        fmt::print(stderr, "ERROR: --csv is only supported for replay mode\n");
        return 1;
    }
    if (csv_output && !analyze) {
        fmt::print(stderr, "ERROR: --csv needs oracle analysis; remove --no-analysis\n");
        return 1;
    }
    if (csv_output)
        print_move_output = false;
    if ((fixed_replay_nodes > 0 || fixed_replay_movetime_ms > 0) && time_mode) {
        fmt::print(stderr, "ERROR: fixed replay budgets cannot be combined with --time\n");
        return 1;
    }

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

    bool compare_reference_requested = reference_path_explicit
                                    && analyze
                                    && analysis_target != "log"
                                    && !csv_output;

    if (run_as_batch)
        return runLogs(collectLogTargets(logfile_targets), argc, argv,
                       logfile_arg_indices, jobs, csv_output,
                       compare_reference_requested);

    if (std::filesystem::path(logfile).extension() != ".log") {
        fmt::print(stderr, "ERROR: replay needs a .log file.\n");
        return 1;
    }

    try {
        std::filesystem::path logfile_path = logfile;
        ParsedLog parsed = readLog(logfile, max_replay_nodes, fixed_replay_nodes,
                                   fixed_replay_movetime_ms);
        std::vector<LogEntry> entries = parsed.entries;
        if (entries.empty()) {
            fmt::print(stderr, "ERROR: No UCI go/bestmove pairs found in '{}'\n", logfile);
            return 1;
        }

        LogEntry final_log_entry = entries.back();
        std::vector<LogEntry> full_log_entries = entries;
        std::string full_log_timeout_report = formatTimeoutReport(entries, final_log_entry.fullmove);
        std::string full_log_game_report = formatGameStatusReport(entries, full_log_timeout_report);
        bool needs_candidate = !analyze || analysis_target != "log";
        bool compare_reference = reference_path_explicit && analysis_target != "log";

        int total_entries = (int)entries.size();
        int display_total = entries.back().fullmove;
        if (print_move_output)
            printBatchBlock(fmt::format("Extracted {} go commands and {} logged moves\n",
                                        total_entries, total_entries));

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

        if (needs_candidate && !executableExists(candidate_path)) {
            fmt::print(stderr, "ERROR: Candidate engine '{}' not found or not executable\n", candidate_path);
            return 1;
        }
        if (compare_reference && !executableExists(reference_path)) {
            fmt::print(stderr, "ERROR: Reference engine '{}' not found or not executable\n", reference_path);
            return 1;
        }

        std::unique_ptr<EngineProcess> oracle;
        if (analyze) {
            if (!executableExists(oracle_path)) {
                fmt::print(stderr, "ERROR: Oracle engine '{}' not found or not executable\n", oracle_path);
                fmt::print(stderr, "Use --oracle <path> or --no-analysis.\n");
                return 1;
            }

            oracle = std::make_unique<EngineProcess>(oracle_path, verbose);
            initializeReference(*oracle);
        }

        if (analyze && analysis_target == "log") {
            bool progress = !csv_output && (isatty(STDOUT_FILENO) || batchMode());
            std::vector<AnalysisEntry> report;
            AnalysisStats stats;
            int analysis_failures = 0;

            bool analyzed_sequence = analyzeLoggedPositionSequence(*oracle,
                                                                   full_log_entries,
                                                                   entries,
                                                                   full_log_range,
                                                                   oracle_limit,
                                                                   display_total,
                                                                   progress,
                                                                   print_move_output,
                                                                   color_output,
                                                                   verbose,
                                                                   report,
                                                                   stats,
                                                                   analysis_failures);
            if (!analyzed_sequence) {
                analyzeLoggedMoves(*oracle, entries, oracle_limit, display_total,
                                   line_widths, progress, print_move_output,
                                   color_output, verbose, report, stats, analysis_failures);
            }

            bool includes_final_log_entry = !entries.empty() && sameLogEntry(entries.back(), final_log_entry);
            std::string timeout_report = includes_final_log_entry ? full_log_timeout_report : "";
            std::string game_report = includes_final_log_entry ? full_log_game_report : "";
            std::string analysis_report_body = formatAnalysisReport(report, stats, analysis_failures,
                                                                    false, true,
                                                                    game_report, timeout_report);

            if (!print_move_output) {
                printBatchBlock(formatDiagnosticsReport(entries, display_total,
                                                        color_output, verbose));
            } else {
                fmt::print("\n");
            }
            printSummaryReport(analysis_report_body, color_output, verbose);
            if (analysis_report_body.empty() || analysis_report_body.back() != '\n')
                fmt::print("\n");

            return reportTriggersFailure(analysis_report_body) ? 1 : 0;
        }

        auto make_engine = [&](const std::string& path) {
            auto engine = std::make_unique<EngineProcess>(path, verbose);
            initializeEngine(*engine, parsed.setoptions, threads);
            return engine;
        };

        std::unique_ptr<EngineProcess> candidate;
        std::unique_ptr<EngineProcess> reference_engine;

        int searched = 0;
        int mismatches = 0;
        double min_wdl = 0.0;
        double max_wdl = 0.0;
        double final_wdl = 0.0;
        bool progress = !csv_output && (isatty(STDOUT_FILENO) || batchMode());
        std::vector<AnalysisEntry> report;
        AnalysisStats stats;
        int analysis_failures = 0;
        std::vector<ComparisonEntry> comparison_report;
        ComparisonStats comparison_stats;
        std::ostringstream csv_buffer;
        std::ostream* csv_stream = nullptr;
        if (csv_output) {
            csv_stream = &csv_buffer;
            writeCsvHeader(*csv_stream);
        }

        for (const auto& entry : entries) {
            if (compare_reference || !candidate)
                candidate = make_engine(candidate_path);

            std::string go_command = time_mode ? entry.logged_go : entry.replay_go;
            candidate->send(entry.position);
            candidate->send(go_command);

            SearchResult result = waitForBestmove(*candidate, entry, go_command, display_total, progress);
            searched++;
            bool mismatch = result.bestmove != entry.expected;
            std::string played_display = formatMoveDisplay(entry.position, result.bestmove);
            std::string expected_display = formatMoveDisplay(entry.position, entry.expected);
            if (mismatch)
                mismatches++;
            min_wdl = std::min(min_wdl, result.wdl);
            max_wdl = std::max(max_wdl, result.wdl);
            final_wdl = result.wdl;

            SearchResult reference_result;
            bool reference_mismatch = false;
            if (compare_reference) {
                reference_engine = make_engine(reference_path);
                reference_engine->send(entry.position);
                reference_engine->send(go_command);
                reference_result = waitForBestmove(*reference_engine, entry,
                                                   go_command, display_total, progress);
                reference_mismatch = reference_result.bestmove != entry.expected;
            }

            std::string reference_suffix;
            if (analyze) {
                if (compare_reference) {
                    ComparisonValidation validation = validateComparison(*oracle,
                                                                        entry.position,
                                                                        result.bestmove,
                                                                        reference_result.bestmove,
                                                                        oracle_limit,
                                                                        entry.depth,
                                                                        entry.fullmove,
                                                                        display_total,
                                                                        progress);
                    if (progress)
                        clearSearchProgress();

                    appendComparisonEntry(comparison_report, comparison_stats,
                                          validation, entry,
                                          result.bestmove, reference_result.bestmove,
                                          display_total);
                    reference_suffix = validation.ok
                        ? fmt::format("reference {} | oracle {} | delta {}",
                                      reference_result.bestmove,
                                      validation.oracle_best,
                                      formatSignedCp(validation.delta_loss))
                        : fmt::format("oracle {}", validation.error);
                    if (csv_stream) {
                        CsvRow row;
                        row.log_path = logfile_path;
                        row.fullmove = entry.fullmove;
                        row.ply_before = validation.ply_before;
                        row.side = sideToMoveName(entry.position);
                        row.log_move = entry.expected;
                        row.candidate_move = result.bestmove;
                        row.reference_move = reference_result.bestmove;
                        row.oracle_best = validation.ok ? validation.oracle_best : "";
                        row.candidate_loss = validation.ok ? validation.candidate_loss : 0;
                        row.reference_loss = validation.ok ? validation.reference_loss : 0;
                        row.delta_loss = validation.ok ? validation.delta_loss : 0;
                        row.candidate_score = validation.ok ? formatScore(validation.candidate_score) : "";
                        row.reference_score = validation.ok ? formatScore(validation.reference_score) : "";
                        row.best_score = validation.ok ? validation.best_score : "";
                        row.candidate_nodes = result.nodes;
                        row.reference_nodes = reference_result.nodes;
                        row.replay_go = go_command;
                        row.logged_nodes = entry.nodes;
                        row.replay_wdl = result.wdl;
                        writeCsvRow(*csv_stream, row);
                    }
                } else {
                    MoveValidation validation = validateMove(*oracle,
                                                             entry.position,
                                                             result.bestmove,
                                                             oracle_limit,
                                                             entry.depth,
                                                             entry.fullmove,
                                                             display_total,
                                                             progress,
                                                             "analyzing",
                                                             true,
                                                             ReferenceScoringMode::RootSearchmoves);
                    if (progress)
                        clearSearchProgress();

                    reference_suffix = formatReferenceInline(validation, color_output);
                    appendAnalysisEntry(report, stats, analysis_failures, validation,
                                        entry, result.bestmove,
                                        mismatch ? entry.expected : "",
                                        display_total);
                    if (csv_stream) {
                        CsvRow row;
                        row.log_path = logfile_path;
                        row.fullmove = entry.fullmove;
                        row.ply_before = validation.ply_before;
                        row.side = sideToMoveName(entry.position);
                        row.log_move = entry.expected;
                        row.candidate_move = result.bestmove;
                        row.oracle_best = validation.ok ? validation.bestmove : "";
                        row.candidate_loss = validation.ok ? validation.cp_loss : 0;
                        row.delta_loss = validation.ok ? validation.cp_loss : 0;
                        row.candidate_score = validation.ok
                            ? formatScore(scoreForSide(validation.after_score_white,
                                                       sideToMoveSign(entry.position)))
                            : "";
                        row.best_score = validation.ok ? validation.best_score : "";
                        row.candidate_nodes = result.nodes;
                        row.replay_go = go_command;
                        row.logged_nodes = entry.nodes;
                        row.replay_wdl = result.wdl;
                        writeCsvRow(*csv_stream, row);
                    }
                }
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

            if (compare_reference || mismatch)
                candidate.reset();
            if (compare_reference || reference_mismatch)
                reference_engine.reset();
        }

        std::string analysis_report_body;
        std::string game_report;
        std::string timeout_report;
        std::string replay_summary_body = formatReplaySummaryReport(searched,
                                                                     mismatches,
                                                                     final_wdl,
                                                                     min_wdl,
                                                                     max_wdl);
        if (analyze) {
            bool includes_final_log_entry = !entries.empty() && sameLogEntry(entries.back(), final_log_entry);
            timeout_report = includes_final_log_entry ? full_log_timeout_report : "";
            game_report = includes_final_log_entry ? full_log_game_report : "";
            if (compare_reference) {
                analysis_report_body = formatComparisonReport(comparison_report, comparison_stats);
            } else {
                analysis_report_body = formatAnalysisReport(report, stats, analysis_failures,
                                                            false, true,
                                                            game_report, timeout_report);
            }
            if (csv_output) {
                std::string csv_report_body = csv_buffer.str();
                fmt::print("{}", csv_report_body);
                if (csv_report_body.empty() || csv_report_body.back() != '\n')
                    fmt::print("\n");
                return 0;
            }

            std::string text_report_body = compare_reference
                ? analysis_report_body
                : replay_summary_body + "\n" + analysis_report_body;
            if (print_move_output)
                printBatchBlock("\n");
            printSummaryReport(text_report_body, color_output, verbose);
        } else {
            if (print_move_output)
                printBatchBlock("\n");
            printBatchBlock(replay_summary_body);
        }

        return reportTriggersFailure(analysis_report_body) ? 1 : 0;
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }

    return 0;
}
