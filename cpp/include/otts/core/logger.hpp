#pragma once

#include <mutex>
#include <string>

namespace otts::core {

class Logger {
public:
    enum class Level {
        Debug = 0,
        Info = 1,
        Warn = 2,
        Error = 3
    };

    static Logger& instance();
    void log(Level level, const std::string& component, const std::string& message);
    void set_level(Level level);
    Level level() const;

private:
    mutable std::mutex mutex_;
    Level threshold_{Level::Info};
};

void log_info(const std::string& component, const std::string& message);
void log_warn(const std::string& component, const std::string& message);
void log_error(const std::string& component, const std::string& message);
void log_debug(const std::string& component, const std::string& message);

}  // namespace otts::core
