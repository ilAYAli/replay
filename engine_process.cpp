#include "engine_process.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <string>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <fmt/format.h>

namespace {

bool startsWith(const std::string& line, const std::string& prefix) {
    return line.rfind(prefix, 0) == 0;
}

bool isDiagnosticLine(const std::string& line) {
    return startsWith(line, "WARNING") || startsWith(line, "ERROR");
}

std::string signalDescription(int signal_number) {
    const char* name = strsignal(signal_number);
    if (!name)
        return fmt::format("signal {}", signal_number);
    return fmt::format("signal {} ({})", signal_number, name);
}

std::optional<std::string> abnormalStatusMessage(int status) {
    if (WIFSIGNALED(status))
        return fmt::format("abnormal termination: engine terminated by {}",
                           signalDescription(WTERMSIG(status)));
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        return fmt::format("abnormal termination: engine exited with status {}",
                           WEXITSTATUS(status));
    return std::nullopt;
}

std::string stripDiagnosticFen(std::string line) {
    for (const std::string& marker : {" FEN: ", " fen="}) {
        size_t pos = line.find(marker);
        if (pos != std::string::npos)
            line.erase(pos);
    }
    return line;
}

} // namespace

struct EngineProcess::Impl {
    FILE* engine_in = nullptr;
    FILE* engine_out = nullptr;
    pid_t pid = -1;
    bool verbose = false;
    bool status_known = false;
    int status = 0;

    bool collectExitStatus(int options) {
        if (status_known)
            return true;
        if (pid <= 0)
            return false;

        int child_status = 0;
        pid_t rc = waitpid(pid, &child_status, options);
        if (rc == pid) {
            pid = -1;
            status = child_status;
            status_known = true;
            return true;
        }
        if (rc < 0 && errno == ECHILD) {
            pid = -1;
            return false;
        }
        return false;
    }
};

EngineProcess::EngineProcess(const std::string& engine_path, bool verbose_output)
    : EngineProcess(std::vector<std::string>{engine_path}, verbose_output) {}

EngineProcess::EngineProcess(const std::vector<std::string>& command, bool verbose_output)
    : impl(std::make_unique<Impl>()) {
    if (command.empty() || command.front().empty())
        throw std::runtime_error("empty engine command");

    impl->verbose = verbose_output;
    signal(SIGPIPE, SIG_IGN);

    int pipe_to_engine[2];
    int pipe_from_engine[2];

    if (pipe(pipe_to_engine) == -1 || pipe(pipe_from_engine) == -1)
        throw std::runtime_error("failed to create pipes");

    impl->pid = fork();
    if (impl->pid == -1)
        throw std::runtime_error("failed to fork");

    if (impl->pid == 0) {
        dup2(pipe_to_engine[0], STDIN_FILENO);
        dup2(pipe_from_engine[1], STDOUT_FILENO);
        if (!impl->verbose) {
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

        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& argument : command)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);

        execvp(argv.front(), argv.data());
        exit(1);
    }

    close(pipe_to_engine[0]);
    close(pipe_from_engine[1]);

    impl->engine_in = fdopen(pipe_to_engine[1], "w");
    impl->engine_out = fdopen(pipe_from_engine[0], "r");

    if (!impl->engine_in || !impl->engine_out)
        throw std::runtime_error("failed to open engine streams");

    setbuf(impl->engine_in, nullptr);
    setbuf(impl->engine_out, nullptr);
}

EngineProcess::~EngineProcess() {
    if (!impl)
        return;

    if (impl->engine_in) {
        fprintf(impl->engine_in, "quit\n");
        fclose(impl->engine_in);
    }
    if (impl->engine_out)
        fclose(impl->engine_out);
    if (impl->pid > 0)
        impl->collectExitStatus(0);

    impl.reset();
}

void EngineProcess::send(const std::string& cmd) {
    if (impl->verbose)
        fmt::print(stderr, "uci:> {}\n", cmd);
    if (fprintf(impl->engine_in, "%s\n", cmd.c_str()) < 0 || fflush(impl->engine_in) == EOF) {
        if (auto abnormal = abnormalTermination())
            throw std::runtime_error(*abnormal);
        throw std::runtime_error(fmt::format("engine stopped while sending command: {}", cmd));
    }
}

bool EngineProcess::waitReadable(int timeout_ms) {
    int fd = fileno(impl->engine_out);
    struct pollfd pfd{fd, POLLIN, 0};
    int rc = poll(&pfd, 1, timeout_ms);
    return rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
}

std::optional<std::string> EngineProcess::readLine(bool print_diagnostics) {
    char buffer[8192];
    if (!fgets(buffer, sizeof(buffer), impl->engine_out)) {
        if (auto abnormal = abnormalTermination())
            throw std::runtime_error(*abnormal);
        return std::nullopt;
    }

    std::string line(buffer);
    if (!line.empty() && line.back() == '\n')
        line.pop_back();
    if (impl->verbose)
        fmt::print(stderr, "{}\n", line);
    else if (print_diagnostics && isDiagnosticLine(line))
        fmt::print(stderr, "{}\n", stripDiagnosticFen(line));
    return line;
}

bool EngineProcess::hasExited() {
    if (impl->pid <= 0)
        return true;

    return impl->collectExitStatus(WNOHANG);
}

std::optional<std::string> EngineProcess::abnormalTermination() {
    impl->collectExitStatus(WNOHANG);
    if (!impl->status_known)
        return std::nullopt;
    return abnormalStatusMessage(impl->status);
}

std::string EngineProcess::terminationMessage(const std::string& context) {
    if (auto abnormal = abnormalTermination())
        return *abnormal + " " + context;
    return "engine exited " + context;
}

void waitForToken(EngineProcess& engine, const std::string& token) {
    while (true) {
        if (!engine.waitReadable(30000)) {
            if (engine.hasExited())
                throw std::runtime_error(engine.terminationMessage(
                    fmt::format("while waiting for {}", token)));
            throw std::runtime_error(fmt::format("timed out waiting for {}", token));
        }

        auto line = engine.readLine();
        if (!line)
            throw std::runtime_error(fmt::format("engine closed stdout while waiting for {}", token));
        if (*line == token)
            return;
    }
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
