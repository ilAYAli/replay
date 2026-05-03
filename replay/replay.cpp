#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <memory>
#include <array>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <fmt/format.h>

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
    int search_time_ms;   // -1 if no info line carried "time"
    std::string side;     // "White" or "Black" (who was to move)
};

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
            
            execl(engine_path.c_str(), engine_path.c_str(), nullptr);
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
    
    std::string waitForBestmove(int move_num, int total_moves, const std::string& current_position, const std::string& expected = "") {
        std::string line;
        int current_depth = 0;
        int current_score = 0;
        int final_score = 0;  // Track final score for bestmove line
        double final_wdl = 0.0;
        bool is_mate = false;
        int mate_sign = 0;

        // Determine side to move from position (count moves in move list)
        size_t moves_pos = current_position.find("moves ");
        int move_count = 0;
        if (moves_pos != std::string::npos) {
            std::string moves_str = current_position.substr(moves_pos + 6);
            move_count = std::count(moves_str.begin(), moves_str.end(), ' ') + 1;
        }
        std::string side = (move_count % 2 == 0) ? "White" : "Black";
        int stm_sign = (move_count % 2 == 0) ? +1 : -1;  // engine score is from side-to-move POV

        while (true) {
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

                // Update progress bar
                int progress_pct = (move_num * 100) / total_moves;

                // Eval in White-POV pawns / mate distance (so sign agrees with WDL).
                std::string eval_str = is_mate
                    ? fmt::format("M{:+d}", final_score * stm_sign)
                    : fmt::format("{:+.2f}", (current_score * stm_sign) / 100.0);

                std::string expecting = expected.empty() ? "" : fmt::format(" | expecting {}", expected);

                if (gui) {
                    fmt::print("\033[s");  // Save cursor
                    fmt::print("\033[24;1H");  // Move to line 24
                    fmt::print("\033[K");  // Clear line

                    fmt::print("{} thinking | Move {}/{} | Depth {} | Eval {} | WDL {:+.2f} [{}%]{}",
                              side, move_num, total_moves, current_depth, eval_str, wdl, progress_pct, expecting);

                    fmt::print("\033[u");  // Restore cursor
                    fflush(stdout);
                } else if (quiet) {
                    fmt::print("\r\033[K{} thinking [{:2}/{:2}] depth {:2} eval {:>6} | WDL {:+.2f} [{:3}%]{}",
                              side, move_num, total_moves, current_depth, eval_str, wdl, progress_pct, expecting);
                    fflush(stdout);
                }
            }
            
            // Filter output
            bool should_print = true;
            if (line.find("root ") == 0) {
                should_print = false;
            } else if ((quiet || gui) && (line.find("info ") == 0 || line.find("info string") == 0)) {
                should_print = false;
            }
            
            if (should_print && !quiet && !gui) {
                std::cout << line << std::endl;
            }
            
            if (line.find("bestmove ") == 0) {
                // Format: "move|score|wdl|mate_in" (mate_in=0 when not a mate score)
                int mate_in = is_mate ? final_score : 0;
                return fmt::format("{}|{}|{:.4f}|{}", extractMove(line), final_score, final_wdl, mate_in);
            }
        }
        return "";
    }
};

int main(int argc, char* argv[]) {
    std::string logfile = "/tmp/enyo.log";
    std::string engine_path = std::string(getenv("HOME")) + "/code/cpp/chess/Enyo/build/enyo";
    bool quiet = true;
    bool gui = false;
    bool print_only = false;
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
            "  --skip N          Skip the first N moves in the log\n"
            "  --moves N         Replay at most N moves (after skipping)\n"
            "  --count N         Alias for --moves\n"
            "  --print           Print the log's bestmoves and exit (no engine run)\n"
            "  --verbose, -v     Print full UCI traffic instead of the compact progress bar\n"
            "  --gui             Show a live board after each move\n"
            "  --help, -h        Show this help and exit\n"
            "\n"
            "Defaults logfile: {}\n",
            prog, engine_path, logfile);
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
        std::cerr << "Failed to open logfile: " << logfile << std::endl;
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
                // Determine side-to-move from the position's moves list.
                size_t mp = current_position.find("moves ");
                int mc = 0;
                if (mp != std::string::npos) {
                    std::string ms = current_position.substr(mp + 6);
                    mc = std::count(ms.begin(), ms.end(), ' ') + 1;
                }
                std::string side = (mc % 2 == 0) ? "White" : "Black";

                commands.push_back(current_position);
                commands.push_back(fmt::format("go depth {}", depth));
                timings.push_back({wtime_ms, btime_ms, last_time_ms, side});
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
        
        // Wait for uciok
        engine.send("uci");
        while (true) {
            line = engine.readLine();
            if (!quiet && !gui) std::cout << line << std::endl;
            if (line == "uciok") break;
        }
        
        // Send setoptions from original game
        for (const auto& opt : setoptions) {
            if (!quiet && !gui) fmt::print("uci:> {}\n", opt);
            engine.send(opt);
        }
        
        int bestmoveIndex = 0;
        int commandIndex = 0;
        int totalCommands = commands.size();
        std::string current_position = "";

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
            engine.send("d");
            while (true) {
                line = engine.readLine();
                if (line.empty() || line.find("Fen:") != std::string::npos) {
                    fmt::print("{}\n", line);
                    if (line.find("Fen:") != std::string::npos) break;
                } else {
                    fmt::print("{}\n", line);
                }
            }
            fmt::print("\n");
        }
        
        for (const auto& cmd : commands) {
            commandIndex++;
            
            if (cmd.find("position ") == 0) {
                current_position = cmd;
            }
            
            if (cmd.find("go ") != std::string::npos) {
                if (!quiet && !gui) {
                    fmt::print("[{}/{}] ", skip + bestmoveIndex + 1, total_moves);
                }
            }

            if (!quiet && !gui) {
                fmt::print("uci:> {}\n", cmd);
            }
            engine.send(cmd);

            if (cmd.find("go ") != std::string::npos) {
                std::string expected_preview = (bestmoveIndex < bestmoves.size()) ? bestmoves[bestmoveIndex] : "";
                std::string result = engine.waitForBestmove(skip + bestmoveIndex + 1, total_moves, current_position, expected_preview);

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

                // Track summary stats
                had_any_move = true;
                if (bestmove != expected) mismatches++;
                if (wdl < min_wdl) min_wdl = wdl;
                if (wdl > max_wdl) max_wdl = wdl;
                final_wdl = wdl;
                final_is_mate = (mate_in != 0);
                final_mate_in = mate_in;
                // Side that just moved: opposite of side-to-move in current_position
                size_t mp = current_position.find("moves ");
                int mc = 0;
                if (mp != std::string::npos) {
                    std::string ms = current_position.substr(mp + 6);
                    mc = std::count(ms.begin(), ms.end(), ' ') + 1;
                }
                final_side = (mc % 2 == 0) ? "White" : "Black";

                if (quiet) {
                    fmt::print("\r\033[K");  // Clear the in-progress "thinking" line
                }

                std::string move_line = (bestmove != expected)
                    ? fmt::format("[{}/{}] bestmove {} (EXPECTED: {}) | WDL {:+.2f}",
                                  skip + bestmoveIndex, total_moves, bestmove, expected, wdl)
                    : fmt::format("[{}/{}] bestmove {} | WDL {:+.2f}",
                                  skip + bestmoveIndex, total_moves, bestmove, wdl);

                if (!gui) {
                    fmt::print("{}\n", move_line);
                }

                // Show board after move in gui mode
                if (gui) {
                    fmt::print("\033[2J\033[H");  // Clear screen and move to top
                    engine.send("d");
                    // Read board output
                    while (true) {
                        line = engine.readLine();
                        if (line.empty() || line.find("Fen:") != std::string::npos) {
                            fmt::print("{}\n", line);
                            if (line.find("Fen:") != std::string::npos) break;
                        } else {
                            fmt::print("{}\n", line);
                        }
                    }
                    fmt::print("\n{}\n", move_line);
                }
            }
        }
        
        engine.send("d");
        while (true) {
            line = engine.readLine();
            if (line.empty()) break;
            std::cout << line << std::endl;
            if (line.find("FEN:") != std::string::npos) break;
        }

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

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
