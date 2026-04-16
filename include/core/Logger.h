#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rf::core {

class Logger {
public:
    using Callback = std::function<void(const std::string&)>;

    static Logger& Instance();

    void SetCallback(Callback callback);
    void Info(const std::string& message);
    void Error(const std::string& message);
    std::vector<std::string> Snapshot() const;

private:
    Logger() = default;

    void Push(const char* level, const std::string& message);

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    Callback callback_;
};

} // namespace rf::core
