#include "core/Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace rf::core {

namespace {

std::string TimestampNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
    localtime_s(&localTime, &timeValue);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%H:%M:%S");
    return stream.str();
}

} // namespace

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetCallback(Callback callback) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
}

void Logger::Info(const std::string& message) {
    Push("INFO", message);
}

void Logger::Error(const std::string& message) {
    Push("ERROR", message);
}

std::vector<std::string> Logger::Snapshot() const {
    std::lock_guard lock(mutex_);
    return lines_;
}

void Logger::Push(const char* level, const std::string& message) {
    const std::string line = "[" + TimestampNow() + "] [" + level + "] " + message;

    Callback callback;
    {
        std::lock_guard lock(mutex_);
        lines_.push_back(line);
        callback = callback_;
    }

    if (callback) {
        callback(line);
    }
}

} // namespace rf::core
