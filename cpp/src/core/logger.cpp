#include "otts/core/logger.hpp"

#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unistd.h>

namespace otts::core {

namespace {

const char* to_string(Logger::Level level) {
    switch (level) {
        case Logger::Level::Info:
            return "INFO";
        case Logger::Level::Warn:
            return "WARN";
        case Logger::Level::Error:
            return "ERROR";
        case Logger::Level::Debug:
            return "DEBUG";
    }
    return "UNKNOWN";
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::log(Level level, const std::string& component, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &time);
#else
    localtime_r(&time, &local_tm);
#endif

    std::ostringstream stream;
    stream << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
           << " [" << to_string(level) << "]"
           << " [" << component << "] "
           << message << '\n';

    const auto line = stream.str();
    std::cout << line;
    std::cout.flush();

    static const char* kLogPath = "/tmp/otts_runtime.log";
    const int fd = ::open(kLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        const auto* data = line.data();
        std::size_t remaining = line.size();
        while (remaining > 0) {
            const auto written = ::write(fd, data, remaining);
            if (written <= 0) {
                break;
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        ::close(fd);
    }
}

void log_info(const std::string& component, const std::string& message) {
    Logger::instance().log(Logger::Level::Info, component, message);
}

void log_warn(const std::string& component, const std::string& message) {
    Logger::instance().log(Logger::Level::Warn, component, message);
}

void log_error(const std::string& component, const std::string& message) {
    Logger::instance().log(Logger::Level::Error, component, message);
}

void log_debug(const std::string& component, const std::string& message) {
    Logger::instance().log(Logger::Level::Debug, component, message);
}

}  // namespace otts::core
