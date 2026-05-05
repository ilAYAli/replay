#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <memory>
#include <array>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <fmt/format.h>

#include "validator.hpp"

int extractDepth(const std::string& line) {
    size_t depthPos = line.find("depth");
    if (depthPos != std::string::npos) {
        size_t startPos = depthPos + 6;
        size_t spacePos = line.find(' ', startPos);
        if (spacePos != std::string::npos) {
            std::string depthStr = line.substr(startPos, spacePos - startPos);
            int depth = std::stoi(depthStr);
            return depth;
        }
    }
    return -1;
}

// Find an integer UCI field by key (whitespace-delimited). Returns -1 if absent
// or unparseable. Ensures we don't match "time" inside "wtime"/"btime".
int parseIntField(const std::string& line, const std::string& key) {
    std::string needle = " " + key + " ";
    size_t pos = line.find(needle);
    size_t start;
    if (pos == std::string::npos) {
        std::string head = key + " ";
        if (line.rfind(head, 0) == 0) {
            start = head.size();
        } else {
            return -1;
        }
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

struct TimingRecord {
    int wtime_ms;         // -1 if absent from go command
    int btime_ms;         // -1 if absent from go command
    int winc_ms;          // -1 if absent
    int binc_ms;          // -1 if absent
    int search_time_ms;   // -1 if no info line carried "time"
    std::string side;     // "White" or "Black" (who was to move)
    std::string original_go;  // the exact `go ...` line from the log (for --time mode)
};

struct ValidationStats {
    int count = 0;
    int failures = 0;
    int quality_total = 0;
    int worst_quality = 100;
    int worst_move_no = 0;
    std::string worst_move;
    std::string worst_label;
};

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

    if (position[board_end + 1] == 'b')
        return -1;
    return +1;
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

std::string appendMoveToPosition(const std::string& position, const std::string& move) {
    if (move.empty() || move == "(none)")
        return position;
    if (position.find(" moves ") != std::string::npos)
        return position + " " + move;
    return position + " moves " + move;
}

std::string extractMove(const std::string& line) {
    size_t bestMovePos = line.find("bestmove ");
    if (bestMovePos != std::string::npos) {
        size_t startPos = bestMovePos + 9;
        std::string move = line.substr(startPos);
        // Remove any trailing whitespace/ponder
        size_t spacePos = move.find(' ');
        if (spacePos != std::string::npos) {
            move = move.substr(0, spacePos);
        }
        return move;
    }
    return "";
}

class EngineProcess {
    FILE* engine_in;
    FILE* engine_out;
    pid_t pid;
    bool quiet;
    bool gui;

public:
    EngineProcess(const std::string& engine_path, bool quiet_mode = false, bool gui_mode = false)
        : quiet(quiet_mode), gui(gui_mode) {
        int pipe_to_engine[2];
        int pipe_from_engine[2];

        if (pipe(pipe_to_engine) == -1 || pipe(pipe_from_engine) == -1) {
            throw std::runtime_error("Failed to create pipes");
        }

        pid = fork();
        if (pid == -1) {
            throw std::runtime_error("Failed to fork");
        }

        if (pid == 0) {
            // Child process
            dup2(pipe_to_engine[0], STDIN_FILENO);
            dup2(pipe_from_engine[1], STDOUT_FILENO);

            // Suppress stderr debug output
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

        // Parent process
        close(pipe_to_engine[0]);
        close(pipe_from_engine[1]);

        engine_in = fdopen(pipe_to_engine[1], "w");
        engine_out = fdopen(pipe_from_engine[0], "r");

        if (!engine_in || !engine_out) {
            throw std::runtime_error("Failed to open streams");
        }

        setbuf(engine_in, nullptr);
        // Also drop stdio buffering on the read side. fgets otherwise pulls
        // multi-line chunks into its own buffer, after which poll() reports
        // "no data" on the kernel pipe even though there are lines queued in
        // stdio — making waitReadable() falsely block for its full timeout
        // and inflating the thinking-line countdown on bursty output.
        setbuf(engine_out, nullptr);
    }

    ~EngineProcess() {
        if (engine_in) {
            fprintf(engine_in, "quit\n");
            fclose(engine_in);
        }
        if (engine_out) fclose(engine_out);
        if (pid > 0) {
            waitpid(pid, nullptr, 0);
        }
    }

    void send(const std::string& cmd) {
        fprintf(engine_in, "%s\n", cmd.c_str());
    }

    // Block up to timeout_ms waiting for engine output. Returns true if data
    // is readable, false on timeout. Used by the thinking-line countdown in
    // --time mode so the UI can tick even when the engine is silent.
    bool waitReadable(int timeout_ms) {
        int fd = fileno(engine_out);
        struct pollfd pfd{fd, POLLIN, 0};
        int rc = poll(&pfd, 1, timeout_ms);
        return rc > 0 && (pfd.revents & POLLIN);
    }

    std::string readLine() {
        char buffer[8192];
        if (fgets(buffer, sizeof(buffer), engine_out)) {
            std::string line(buffer);
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            return line;
        }
        return "";
    }

    std::string waitForBestmove(int move_num, int total_moves, const std::string& current_position, const std::string& expected = "", int budget_ms = -1) {
        std::string line;
        int current_depth = 0;
        int current_score = 0;
        int final_score = 0;  // Track final score for bestmove line
        double final_wdl = 0.0;
        bool is_mate = false;
        int mate_sign = 0;

        std::string side = sideToMoveName(current_position);
        int stm_sign = sideToMoveSign(current_position);  // engine score is from side-to-move POV

        // State used for the thinking-line countdown in --time mode. We
        // remember the last eval/depth so a tick-driven repaint (when the
        // engine is silent) can show the same fields as an info-driven one.
        auto t_start = std::chrono::steady_clock::now();
        std::string last_eval_str = "  0.00";
        auto paint_thinking = [&](double wdl) {
            if (!quiet && !gui) return;
            std::string expecting = expected.empty() ? "" : fmt::format(" | expecting {}", expected);
            std::string countdown;
            if (budget_ms > 0) {
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_start).count();
                countdown = fmt::format(" | {:5.1f}s/{:.1f}s",
                                        elapsed_ms / 1000.0, budget_ms / 1000.0);
            }
            if (gui) {
                fmt::print("\033[s\033[24;1H\033[K");
                fmt::print("{} thinking | Move {}/{} | Depth {} | Eval {} | WDL {:+.2f}{}{}",
                           side, move_num, total_moves, current_depth, last_eval_str,
                           wdl, countdown, expecting);
                fmt::print("\033[u");
            } else {
                fmt::print("\r\033[K{} thinking [{:2}/{:2}] depth {:2} eval {:>6} | WDL {:+.2f}{}{}",
                           side, move_num, total_moves, current_depth, last_eval_str,
                           wdl, countdown, expecting);
            }
            fflush(stdout);
        };

        while (true) {
            // Tick the thinking line every 1s even if the engine is silent,
            // so stalls are visible in --time mode. Without a budget, fall
            // back to a plain blocking read (no countdown needed).
            if (budget_ms > 0) {
                while (!waitReadable(1000)) {
                    paint_thinking(final_wdl);
                }
            }
            line = readLine();
            if (line.empty()) break;

            // Extract depth and score for progress bar
            if (line.find("info depth ") != std::string::npos) {
                current_depth = extractDepth(line);
                size_t mate_pos = line.find("score mate ");
                size_t score_pos = line.find("score cp ");
                if (mate_pos != std::string::npos) {
                    size_t start = mate_pos + 11;
                    size_t end = line.find(' ', start);
                    int mate_in = std::stoi(line.substr(start, end - start));
                    is_mate = true;
                    mate_sign = (mate_in >= 0) ? +1 : -1;
                    final_score = mate_in;
                } else if (score_pos != std::string::npos) {
                    size_t start = score_pos + 9;
                    size_t end = line.find(' ', start);
                    current_score = std::stoi(line.substr(start, end - start));
                    final_score = current_score;  // Track final eval
                    is_mate = false;
                }

                // Normalize to -1..+1 from White's POV.
                // Engine reports score from side-to-move POV, so flip for Black.
                double wdl;
                if (is_mate) {
                    wdl = mate_sign * stm_sign;  // ±1
                } else {
                    // tanh gives a smooth mapping; ~±600cp saturates near ±1.
                    wdl = std::tanh((current_score * stm_sign) / 300.0);
                }
                final_wdl = wdl;

                last_eval_str = is_mate
                    ? fmt::format("M{:+d}", final_score * stm_sign)
                    : fmt::format("{:+.2f}", (current_score * stm_sign) / 100.0);
                paint_thinking(wdl);
            }

            // Filter output
            bool should_print = true;
            if (line.find("root ") == 0) {
                should_print = false;
            } else if ((quiet || gui) && (line.find("info ") == 0 || line.find("info string") == 0)) {
                should_print = false;
            }

            if (should_print && !quiet && !gui) {
                fmt::print("{}\n", line);
            }

            if (line.find("bestmove ") == 0) {
                // Format: "move|score|wdl|mate_in" (mate_in=0 when not a mate score)
                int mate_in = is_mate ? final_score : 0;
                return fmt::format("{}|{}|{:.4f}|{}", extractMove(line), final_score, final_wdl, mate_in);
            }
        }
        return "";
    }

    void printPgn() {
        send("pgn");
        send("isready");
        while (true) {
            if (!waitReadable(30000))
                throw std::runtime_error("Timed out waiting for PGN output");

            std::string line = readLine();
            if (line == "readyok")
                break;
            fmt::print("{}\n", line);
        }
    }
};

void waitForUciToken(EngineProcess& engine, const std::string& token, bool verbose) {
    while (true) {
        if (!engine.waitReadable(30000))
            throw std::runtime_error(fmt::format("Timed out waiting for {}", token));
        std::string line = engine.readLine();
        if (line.empty())
            continue;
        if (verbose)
            fmt::print("{}\n", line);
        if (line == token)
            break;
    }
}

void initializeUciEngine(EngineProcess& engine, bool verbose) {
    engine.send("uci");
    waitForUciToken(engine, "uciok", verbose);
}

void waitForReady(EngineProcess& engine, bool verbose) {
    engine.send("isready");
    waitForUciToken(engine, "readyok", verbose);
}

bool isFenLine(const std::string& line) {
    return line.find("Fen:") != std::string::npos || line.find("FEN:") != std::string::npos;
}

void printBoard(EngineProcess& engine) {
    engine.send("d");
    while (true) {
        if (!engine.waitReadable(30000))
            throw std::runtime_error("Timed out waiting for board output");

        std::string line = engine.readLine();
        fmt::print("{}\n", line);
        if (isFenLine(line))
            break;
    }
}

int main(int argc, char* argv[]) {
    std::string logfile = "/tmp/enyo.log";
    std::string engine_path = std::string(getenv("HOME")) + "/code/cpp/chess/enyo/build/enyo";
    std::string validator_path = "stockfish";
    bool quiet = true;
    bool gui = false;
    bool print_only = false;
    bool print_pgn = false;
    bool time_mode = false;  // --time: replay original `go wtime...` instead of `go depth N`
    bool validate = false;
    bool color = false;
    int threads = -1;    // -1 = don't send setoption; otherwise override engine default
    int skip = 0;
    int max_moves = -1;  // -1 = replay all remaining

    auto print_help = [&](const char* prog) {
        fmt::print(
            "Usage: {} [options] <logfile>\n"
            "\n"
            "Replays the UCI go/bestmove pairs from an Enyo engine logfile,\n"
            "comparing the engine's current bestmoves against the logged ones.\n"
            "\n"
            "Options:\n"
            "  --engine <path>   Path to the engine binary (default: {})\n"
            "  --skip N          Skip the first N moves in the log. NOTE: this jumps\n"
            "                    straight to move N+1 with a fresh engine, so TT,\n"
            "                    history, and time state do NOT match the original\n"
            "                    run. Do not use --skip to reproduce state-dependent\n"
            "                    bugs (fallbacks, time-management, TT-driven moves).\n"
            "  --moves N         Replay at most N moves (after skipping)\n"
            "  --count N         Alias for --moves\n"
            "  --print           Print the log's bestmoves and exit (no engine run)\n"
            "  --pgn             Print PGN for the replayed logged moves at the end\n"
            "  --time            Replay with the original `go wtime X btime Y winc Z binc W`\n"
            "                    command from the log, not `go depth N`. Needed to reproduce\n"
            "                    timeout-driven fallbacks (e.g. empty-PV bestmove fallbacks).\n"
            "  --threads N       Send `setoption name Threads value N` at startup. Enyo logs\n"
            "                    usually do not record the thread count the engine was launched\n"
            "                    with, so reproducing multi-threaded pathologies requires\n"
            "                    passing this explicitly.\n"
            "  --validate        Validate each logged move with Stockfish at the logged depth\n"
            "  --validator <path>\n"
            "                    UCI validator engine for --validate (default: {})\n"
            "  --color           Colorize score percentages\n"
            "  --verbose, -v     Print full UCI traffic instead of the compact progress bar\n"
            "  --gui             Show a live board after each move\n"
            "  --help, -h        Show this help and exit\n"
            "\n"
            "Defaults logfile: {}\n",
            prog, engine_path, validator_path, logfile);
    };

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "--engine" && i + 1 < argc) {
            engine_path = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            quiet = false;
        } else if (arg == "--gui") {
            gui = true;
        } else if (arg == "--print") {
            print_only = true;
        } else if (arg == "--pgn") {
            print_pgn = true;
        } else if (arg == "--time") {
            time_mode = true;
        } else if (arg == "--validate") {
            validate = true;
        } else if (arg == "--color") {
            color = true;
        } else if ((arg == "--validator" || arg == "--validate-engine") && i + 1 < argc) {
            validate = true;
            validator_path = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
            if (threads < 1) threads = 1;
        } else if (arg == "--skip" && i + 1 < argc) {
            skip = std::stoi(argv[++i]);
            if (skip < 0) skip = 0;
        } else if ((arg == "--moves" || arg == "--count") && i + 1 < argc) {
            max_moves = std::stoi(argv[++i]);
            if (max_moves < 0) max_moves = 0;
        } else if (arg.rfind("--", 0) == 0 || arg == "-v" || arg == "-h") {
            fmt::print(stderr, "Unknown or malformed option: {}\n", arg);
            print_help(argv[0]);
            return 1;
        } else {
            logfile = arg;
        }
    }

    std::ifstream file(logfile);
    if (!file.is_open()) {
        fmt::print(stderr, "Failed to open logfile: {}\n", logfile);
        return 1;
    }

    std::vector<std::string> commands;
    std::vector<std::string> bestmoves;
    std::string line;
    std::streampos firstUciNewGamePos = -1;

    // First pass: find first ucinewgame
    while (std::getline(file, line)) {
        if (line == "ucinewgame" && firstUciNewGamePos == -1) {
            firstUciNewGamePos = file.tellg();
        }
    }

    file.clear();
    file.seekg(0, std::ios::beg);

    if (firstUciNewGamePos != -1) {
        file.seekg(firstUciNewGamePos);
    }

    // Second pass: extract all bestmoves
    std::vector<std::string> setoptions;
    while (std::getline(file, line)) {
        if (line.find("bestmove ") == 0) {
            bestmoves.push_back(extractMove(line));
        }
        if (line.find("setoption ") == 0) {
            setoptions.push_back(line);
        }
    }

    file.clear();
    file.seekg(0, std::ios::beg);

    if (firstUciNewGamePos != -1) {
        file.seekg(firstUciNewGamePos);
    }

    // Third pass: extract position+go pairs with depths (and timing data)
    std::string current_position = "";
    std::vector<TimingRecord> timings;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::string command = line.substr(0, line.find(' '));

        if (command == "position") {
            current_position = line;
        } else if (command == "go") {
            // Capture time-control fields from the original `go` line.
            int wtime_ms = parseIntField(line, "wtime");
            int btime_ms = parseIntField(line, "btime");
            int winc_ms  = parseIntField(line, "winc");
            int binc_ms  = parseIntField(line, "binc");
            std::string original_go = line;

            // Scan forward for info depth lines and confirm a bestmove follows
            // before the next go/position/ucinewgame.
            std::string depth_str;
            int depth = 0;
            int last_time_ms = -1;
            bool saw_bestmove = false;
            std::streampos start_pos = file.tellg();
            while (std::getline(file, depth_str)) {
                if (depth_str.find("info depth ") != std::string::npos) {
                    int d = extractDepth(depth_str);
                    if (d > depth) depth = d;
                    int t = parseIntField(depth_str, "time");
                    if (t >= 0) last_time_ms = t;
                }
                if (!depth_str.empty()) {
                    std::string next_cmd = depth_str.substr(0, std::min(depth_str.find(' '), depth_str.size()));
                    if (next_cmd == "bestmove") {
                        saw_bestmove = true;
                        file.seekg(start_pos);
                        break;
                    }
                    if (next_cmd == "position" || next_cmd == "go" || next_cmd == "ucinewgame") {
                        file.seekg(start_pos);
                        break;
                    }
                }
                start_pos = file.tellg();
            }

            if (saw_bestmove && !current_position.empty()) {
                // Some positions produce a bestmove with no `info depth` line
                // (engine fallback / forced move). Replay with depth 1 so the
                // go/bestmove count stays in lockstep with the log.
                if (depth <= 0) depth = 1;
                std::string side = sideToMoveName(current_position);

                commands.push_back(current_position);
                commands.push_back(fmt::format("go depth {}", depth));
                timings.push_back({wtime_ms, btime_ms, winc_ms, binc_ms,
                                   last_time_ms, side, original_go});
                current_position = ""; // Clear so we don't add same position twice
            }
        }
    }

    file.close();

    int go_cmd_count = std::count_if(commands.begin(), commands.end(),
                                      [](const std::string& s) { return s.find("go depth") == 0; });

    if (go_cmd_count == 0 && bestmoves.empty()) {
        fmt::print(stderr, "ERROR: No UCI go/bestmove pairs found in '{}' — not an Enyo logfile?\n", logfile);
        return 1;
    }

    fmt::print("Extracted {} go commands and {} bestmoves\n", go_cmd_count, bestmoves.size());

    if (go_cmd_count != bestmoves.size()) {
        fmt::print("First 10 commands:\n");
        for (int i = 0; i < std::min(10, (int)commands.size()); i++) {
            fmt::print("  {}: {}\n", i, commands[i]);
        }
        fmt::print("ERROR: Mismatch between go commands ({}) and bestmoves ({})\n", go_cmd_count, bestmoves.size());
        return 1;
    }

    int total_moves = (int)bestmoves.size();  // original count, preserved for display

    if (skip > 0) {
        if (skip >= total_moves) {
            fmt::print(stderr, "ERROR: --skip {} >= total moves {}\n", skip, total_moves);
            return 1;
        }
        // commands is [position, go, position, go, ...] — drop 2*skip entries
        commands.erase(commands.begin(), commands.begin() + 2 * skip);
        bestmoves.erase(bestmoves.begin(), bestmoves.begin() + skip);
        if ((int)timings.size() >= skip) {
            timings.erase(timings.begin(), timings.begin() + skip);
        }
        fmt::print("Skipping first {} moves; {} remaining\n", skip, bestmoves.size());
    }

    if (max_moves >= 0 && (int)bestmoves.size() > max_moves) {
        commands.erase(commands.begin() + 2 * max_moves, commands.end());
        bestmoves.erase(bestmoves.begin() + max_moves, bestmoves.end());
        if ((int)timings.size() > max_moves) {
            timings.erase(timings.begin() + max_moves, timings.end());
        }
        fmt::print("Limiting to {} moves\n", max_moves);
    }

    if (print_only) {
        for (size_t m = 0; m < bestmoves.size(); ++m) {
            fmt::print("[{}/{}] {}\n", skip + (int)m + 1, total_moves, bestmoves[m]);
        }
        return 0;
    }

    if (!quiet && !gui) {
        fmt::print("Using engine: {}\n", engine_path);
        fmt::print("Total moves to replay: {}\n\n", bestmoves.size());
    }

    try {
        EngineProcess engine(engine_path, quiet, gui);

        initializeUciEngine(engine, !quiet && !gui);

        // Send setoptions from original game
        for (const auto& opt : setoptions) {
            if (!quiet && !gui) fmt::print("uci:> {}\n", opt);
            engine.send(opt);
        }

        // --threads overrides whatever the log did (or didn't) record. Sent
        // last so it wins. Required to reproduce multi-threaded pathologies
        // from logs that don't carry the launcher's --threads flag.
        if (threads > 0) {
            std::string opt = fmt::format("setoption name Threads value {}", threads);
            if (!quiet && !gui) fmt::print("uci:> {}\n", opt);
            engine.send(opt);
        }

        std::unique_ptr<ValidatorWorker> validator;
        if (validate) {
            if (!quiet && !gui)
                fmt::print("Using validator: {}\n", validator_path);
            validator = std::make_unique<ValidatorWorker>(validator_path);
        }

        int bestmoveIndex = 0;
        int commandIndex = 0;
        int totalCommands = commands.size();
        std::string current_position = "";
        std::string pgn_position = "";
        ValidationStats validation_stats;

        int mismatches = 0;
        double min_wdl = 0.0;   // worst for White
        double max_wdl = 0.0;   // best for White
        double final_wdl = 0.0;
        bool final_is_mate = false;
        int final_mate_in = 0;
        std::string final_side = "";
        bool had_any_move = false;

        // Show initial board in gui mode
        if (gui && !commands.empty()) {
            // Find and send first position
            for (const auto& cmd : commands) {
                if (cmd.find("position ") == 0) {
                    engine.send(cmd);
                    break;
                }
            }

            fmt::print("\033[2J\033[H");  // Clear screen
            printBoard(engine);
            fmt::print("\n");
        }

        for (const auto& cmd : commands) {
            commandIndex++;

            if (cmd.find("position ") == 0) {
                current_position = cmd;
            }

            // In --time mode, swap the depth-N command for the original
            // `go wtime X btime Y ...` line recorded from the log. This
            // reproduces the engine's hard-time behavior, which is the only
            // way to trigger timeout-driven fallbacks (empty-PV bestmove).
            std::string send_cmd = cmd;
            std::string expected_preview;
            std::future<MoveValidation> pending_validation;
            int pending_validation_depth = -1;
            if (cmd.find("go ") != std::string::npos) {
                if (time_mode
                 && bestmoveIndex < (int)timings.size()
                 && !timings[bestmoveIndex].original_go.empty()) {
                    send_cmd = timings[bestmoveIndex].original_go;
                }
                expected_preview = (bestmoveIndex < bestmoves.size()) ? bestmoves[bestmoveIndex] : "";
                if (validate && validator && !expected_preview.empty()) {
                    pending_validation_depth = std::max(1, parseIntField(cmd, "depth"));
                    pending_validation = validator->submit(current_position, expected_preview, pending_validation_depth);
                }
                if (!quiet && !gui) {
                    fmt::print("[{}/{}] ", skip + bestmoveIndex + 1, total_moves);
                }
            }

            if (!quiet && !gui) {
                fmt::print("uci:> {}\n", send_cmd);
            }
            engine.send(send_cmd);

            if (cmd.find("go ") != std::string::npos) {
                // Pass the side's remaining clock as the budget in --time mode
                // so the thinking line can show a live countdown. The engine
                // enforces its own hard-time; budget here is cosmetic only.
                int budget_ms = -1;
                if (time_mode && bestmoveIndex < (int)timings.size()) {
                    const auto& t = timings[bestmoveIndex];
                    budget_ms = (t.side == "White") ? t.wtime_ms : t.btime_ms;
                }
                std::string result = engine.waitForBestmove(skip + bestmoveIndex + 1, total_moves, current_position, expected_preview, budget_ms);

                // Parse bestmove|score|wdl|mate_in format
                std::vector<std::string> parts;
                size_t start = 0;
                while (true) {
                    size_t p = result.find('|', start);
                    parts.push_back(result.substr(start, p == std::string::npos ? p : p - start));
                    if (p == std::string::npos) break;
                    start = p + 1;
                }
                std::string bestmove = parts.size() > 0 ? parts[0] : "";
                double wdl = parts.size() > 2 ? std::stod(parts[2]) : 0.0;
                int mate_in = parts.size() > 3 ? std::stoi(parts[3]) : 0;

                if (bestmoveIndex >= bestmoves.size()) {
                    fmt::print("ERROR: No expected bestmove for position {}\n", bestmoveIndex);
                    break;
                }

                std::string expected = bestmoves[bestmoveIndex++];
                int move_no = skip + bestmoveIndex;
                pgn_position = appendMoveToPosition(current_position, expected);

                // Track summary stats
                had_any_move = true;
                if (bestmove != expected) mismatches++;
                if (wdl < min_wdl) min_wdl = wdl;
                if (wdl > max_wdl) max_wdl = wdl;
                final_wdl = wdl;
                final_is_mate = (mate_in != 0);
                final_mate_in = mate_in;
                final_side = sideToMoveName(current_position);

                if (quiet) {
                    fmt::print("\r\033[K");  // Clear the in-progress "thinking" line
                }

                MoveValidation move_validation;
                if (validate && pending_validation.valid()) {
                    if (quiet && !gui
                     && pending_validation.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                        fmt::print("\r\033[KSF validating [{:2}/{:2}] depth {:2}",
                                   move_no, total_moves, pending_validation_depth);
                        fflush(stdout);
                    }

                    move_validation = pending_validation.get();

                    if (quiet && !gui)
                        fmt::print("\r\033[K");

                    if (move_validation.ok) {
                        validation_stats.count++;
                        validation_stats.quality_total += move_validation.quality;
                        if (move_validation.quality < validation_stats.worst_quality) {
                            validation_stats.worst_quality = move_validation.quality;
                            validation_stats.worst_move_no = move_no;
                            validation_stats.worst_move = expected;
                            validation_stats.worst_label = move_validation.label;
                        }
                    } else {
                        validation_stats.failures++;
                    }
                }

                std::string move_line = (bestmove != expected)
                    ? fmt::format("[{}/{}] bestmove {} (EXPECTED: {}) | WDL {:+.2f}",
                                  move_no, total_moves, bestmove, expected, wdl)
                    : fmt::format("[{}/{}] bestmove {} | WDL {:+.2f}",
                                  move_no, total_moves, bestmove, wdl);
                if (validate)
                    move_line += formatValidation(move_validation, color);

                if (!gui) {
                    fmt::print("{}\n", move_line);
                }

                // Show board after move in gui mode
                if (gui) {
                    fmt::print("\033[2J\033[H");  // Clear screen and move to top
                    printBoard(engine);
                    fmt::print("\n{}\n", move_line);
                }
            }
        }

        printBoard(engine);

        if (had_any_move) {
            auto wdl_label = [](double w) {
                if (w >=  0.90) return "White winning";
                if (w >=  0.50) return "White clearly better";
                if (w >=  0.20) return "White better";
                if (w >  -0.20) return "Roughly equal";
                if (w >  -0.50) return "Black better";
                if (w >  -0.90) return "Black clearly better";
                return "Black winning";
            };

            int matched = bestmoveIndex - mismatches;
            fmt::print("\n=== Summary ===\n");
            fmt::print("Positions replayed : {}\n", bestmoveIndex);
            fmt::print("Bestmove matches   : {}/{} ({} differed)\n",
                       matched, bestmoveIndex, mismatches);
            fmt::print("Final WDL          : {:+.2f} ({}, {} to move next)\n",
                       final_wdl, wdl_label(final_wdl),
                       final_side == "White" ? "Black" : "White");
            fmt::print("WDL range          : [{:+.2f}, {:+.2f}]\n", min_wdl, max_wdl);
            if (final_is_mate) {
                int plies = std::abs(final_mate_in);
                // Engine's mate score is from side-to-move's POV; side-to-move
                // is the opposite of whoever just moved.
                std::string stm = (final_side == "White") ? "Black" : "White";
                std::string winner = (final_mate_in > 0) ? stm : final_side;
                fmt::print("Mate               : {} mates in {} {}\n",
                           winner, plies, plies == 1 ? "ply" : "plies");
            }

            if (validate) {
                if (validation_stats.count > 0) {
                    double avg_quality = validation_stats.quality_total / (double)validation_stats.count;
                    fmt::print("SF score           : avg {}, worst {}\n",
                               formatQualityPercent(avg_quality, color),
                               formatQualityPercent(validation_stats.worst_quality, color));
                    fmt::print("Worst SF move      : {}. {} ({})\n",
                               validation_stats.worst_move_no, validation_stats.worst_move,
                               validation_stats.worst_label);
                }
                if (validation_stats.failures > 0)
                    fmt::print("SF failures        : {}\n", validation_stats.failures);
            }

            // Time-overrun check: flag moves whose search time met or exceeded
            // the side's remaining clock at the start of the `go` command.
            std::vector<std::pair<int, const TimingRecord*>> overruns;
            for (size_t m = 0; m < timings.size() && (int)m < bestmoveIndex; ++m) {
                const TimingRecord& t = timings[m];
                int budget = (t.side == "White") ? t.wtime_ms : t.btime_ms;
                if (budget <= 0 || t.search_time_ms < 0) continue;
                if (t.search_time_ms >= budget) {
                    overruns.push_back({skip + (int)m + 1, &t});
                }
            }
            if (!overruns.empty()) {
                fmt::print("Time overruns      : {}\n", overruns.size());
                for (const auto& [move_no, t] : overruns) {
                    int budget = (t->side == "White") ? t->wtime_ms : t->btime_ms;
                    fmt::print("  WARNING: Move {} ({}) used {}ms of {}ms remaining\n",
                               move_no, t->side, t->search_time_ms, budget);
                }
            }
        }

        if (print_pgn && !pgn_position.empty()) {
            fmt::print("\n=== PGN ===\n");
            engine.send(pgn_position);
            engine.printPgn();
        }

    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }

    return 0;
}
