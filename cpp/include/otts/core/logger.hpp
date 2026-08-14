#pragma once

#include <mutex>
#include <string>

namespace otts::core {

class Logger {
public:
    enum class Level {
        Info,
        Warn,
        Error,
        Debug
    };

    static Logger& instance();
    void log(Level level, const std::string& component, const std::string& message);

private:
    std::mutex mutex_;
};

void log_info(const std::string& component, const std::string& message);
void log_warn(const std::string& component, const std::string& message);
void log_error(const std::string& component, const std::string& message);
void log_debug(const std::string& component, const std::string& message);

}  // namespace otts::core
