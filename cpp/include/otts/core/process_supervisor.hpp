#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace otts::core {

struct ManagedProcessSnapshot {
    std::string name;
    int pid{-1};
    bool running{false};
    std::uint64_t started_at_epoch_ms{0};
    std::string workdir;
    std::string stdout_path;
    std::string stderr_path;
};

class ManagedProcess {
public:
    ManagedProcess() = default;
    ~ManagedProcess();

    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    bool start(
        const std::string& name,
        const std::string& workdir,
        const std::vector<std::string>& argv,
        const std::vector<std::string>& env_overrides,
        const std::string& stdout_path,
        const std::string& stderr_path);
    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] ManagedProcessSnapshot snapshot() const;

private:
    std::string name_;
    std::string workdir_;
    std::string stdout_path_;
    std::string stderr_path_;
    int pid_{-1};
    std::uint64_t started_at_epoch_ms_{0};
};

}  // namespace otts::core
