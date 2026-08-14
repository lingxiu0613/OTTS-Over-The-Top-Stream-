#include "otts/core/process_supervisor.hpp"

#include "otts/core/logger.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace otts::core {

ManagedProcess::~ManagedProcess() {
    stop();
}

bool ManagedProcess::start(
    const std::string& name,
    const std::string& workdir,
    const std::vector<std::string>& argv,
    const std::vector<std::string>& env_overrides,
    const std::string& stdout_path,
    const std::string& stderr_path) {
    name_ = name;
    workdir_ = workdir;
    stdout_path_ = stdout_path;
    stderr_path_ = stderr_path;
    started_at_epoch_ms_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

#ifdef _WIN32
    (void)workdir;
    (void)argv;
    (void)env_overrides;
    (void)stdout_path;
    (void)stderr_path;
    otts::core::log_warn("process", "managed process launch is only implemented on Linux for " + name);
    return false;
#else
    if (argv.empty()) {
        otts::core::log_error("process", "empty argv for " + name);
        return false;
    }

    const auto child_pid = ::fork();
    if (child_pid < 0) {
        otts::core::log_error("process", "fork failed for " + name + ": " + std::strerror(errno));
        return false;
    }

    if (child_pid == 0) {
        ::setsid();

        if (!workdir.empty()) {
            ::chdir(workdir.c_str());
        }

        const int out_fd = ::open(stdout_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        const int err_fd = ::open(stderr_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (out_fd >= 0) {
            ::dup2(out_fd, STDOUT_FILENO);
            ::close(out_fd);
        }
        if (err_fd >= 0) {
            ::dup2(err_fd, STDERR_FILENO);
            ::close(err_fd);
        }

        for (int fd = 3; fd < 256; ++fd) {
            ::close(fd);
        }

        for (const auto& item : env_overrides) {
            const auto pos = item.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            ::setenv(item.substr(0, pos).c_str(), item.substr(pos + 1).c_str(), 1);
        }

        std::vector<char*> exec_argv;
        exec_argv.reserve(argv.size() + 1);
        for (const auto& item : argv) {
            exec_argv.push_back(const_cast<char*>(item.c_str()));
        }
        exec_argv.push_back(nullptr);

        ::execvp(exec_argv.front(), exec_argv.data());
        std::fprintf(stderr, "execvp failed for %s: %s\n", name.c_str(), std::strerror(errno));
        _exit(127);
    }

    pid_ = static_cast<int>(child_pid);
    otts::core::log_info("process", "started " + name + " pid=" + std::to_string(pid_));
    return true;
#endif
}

void ManagedProcess::stop() {
#ifndef _WIN32
    if (pid_ <= 0) {
        return;
    }
    ::kill(pid_, SIGTERM);
    for (int attempt = 0; attempt < 20; ++attempt) {
        int status = 0;
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return;
        }
        ::usleep(100 * 1000);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, nullptr, 0);
    otts::core::log_info("process", "stopped " + name_);
    pid_ = -1;
#endif
}

bool ManagedProcess::running() const {
#ifndef _WIN32
    if (pid_ <= 0) {
        return false;
    }
    return ::kill(pid_, 0) == 0;
#else
    return false;
#endif
}

ManagedProcessSnapshot ManagedProcess::snapshot() const {
    ManagedProcessSnapshot snapshot;
    snapshot.name = name_;
    snapshot.pid = pid_;
    snapshot.running = running();
    snapshot.started_at_epoch_ms = started_at_epoch_ms_;
    snapshot.workdir = workdir_;
    snapshot.stdout_path = stdout_path_;
    snapshot.stderr_path = stderr_path_;
    return snapshot;
}

}  // namespace otts::core
