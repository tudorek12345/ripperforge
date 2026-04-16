#include "core/Settings.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

namespace rf::core {

namespace {

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }

    std::wstring result(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

std::string EscapeJson(std::string value) {
    std::string escaped;
    escaped.reserve(value.size());

    for (const char c : value) {
        if (c == '\\' || c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }

    return escaped;
}

std::string UnescapeJson(const std::string& value) {
    std::string unescaped;
    unescaped.reserve(value.size());

    bool escaped = false;
    for (const char c : value) {
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }

        unescaped.push_back(c);
        escaped = false;
    }

    return unescaped;
}

std::optional<std::string> MatchValue(const std::string& input, const std::regex& expression) {
    std::smatch match;
    if (!std::regex_search(input, match, expression) || match.size() < 2) {
        return std::nullopt;
    }

    return match[1].str();
}

} // namespace

AppSettings LoadSettings(const std::wstring& filePath) {
    AppSettings settings;

    std::ifstream stream(std::filesystem::path(filePath), std::ios::binary);
    if (!stream.good()) {
        return settings;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();

    static const std::regex autoRefreshRegex(R"("autoRefresh"\s*:\s*(true|false))");
    static const std::regex intervalRegex(R"("refreshIntervalMs"\s*:\s*(\d+))");
    static const std::regex lastDllRegex(R"("lastDllPath"\s*:\s*"([^"]*)")");
    static const std::regex filterRegex(R"("processFilter"\s*:\s*"([^"]*)")");
    static const std::regex captureDllRegex(R"("captureDllPath"\s*:\s*"([^"]*)")");
    static const std::regex captureOutputRegex(R"("captureOutputDir"\s*:\s*"([^"]*)")");
    static const std::regex lastTextureRegex(R"("lastTextureAssetPath"\s*:\s*"([^"]*)")");
    static const std::regex lastModelRegex(R"("lastModelAssetPath"\s*:\s*"([^"]*)")");
    static const std::regex hookDllRegex(R"("hookDllPath"\s*:\s*"([^"]*)")");
    static const std::regex hookBackendRegex(R"("hookBackend"\s*:\s*"([^"]*)")");

    if (auto autoRefreshMatch = MatchValue(content, autoRefreshRegex)) {
        settings.autoRefresh = (*autoRefreshMatch == "true");
    }

    if (auto intervalMatch = MatchValue(content, intervalRegex)) {
        settings.refreshIntervalMs = static_cast<uint32_t>(std::stoul(*intervalMatch));
    }

    if (auto dllMatch = MatchValue(content, lastDllRegex)) {
        settings.lastDllPath = Utf8ToWide(UnescapeJson(*dllMatch));
    }

    if (auto filterMatch = MatchValue(content, filterRegex)) {
        settings.processFilter = Utf8ToWide(UnescapeJson(*filterMatch));
    }

    if (auto captureDllMatch = MatchValue(content, captureDllRegex)) {
        settings.captureDllPath = Utf8ToWide(UnescapeJson(*captureDllMatch));
    }

    if (auto captureOutMatch = MatchValue(content, captureOutputRegex)) {
        settings.captureOutputDir = Utf8ToWide(UnescapeJson(*captureOutMatch));
    }

    if (auto textureMatch = MatchValue(content, lastTextureRegex)) {
        settings.lastTextureAssetPath = Utf8ToWide(UnescapeJson(*textureMatch));
    }

    if (auto modelMatch = MatchValue(content, lastModelRegex)) {
        settings.lastModelAssetPath = Utf8ToWide(UnescapeJson(*modelMatch));
    }

    if (auto hookDllMatch = MatchValue(content, hookDllRegex)) {
        settings.hookDllPath = Utf8ToWide(UnescapeJson(*hookDllMatch));
    }

    if (auto hookBackendMatch = MatchValue(content, hookBackendRegex)) {
        settings.hookBackend = Utf8ToWide(UnescapeJson(*hookBackendMatch));
    }

    return settings;
}

bool SaveSettings(const std::wstring& filePath, const AppSettings& settings) {
    std::error_code errorCode;
    const std::filesystem::path targetPath(filePath);
    std::filesystem::create_directories(targetPath.parent_path(), errorCode);

    std::ofstream stream(std::filesystem::path(filePath), std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
        return false;
    }

    stream << "{\n";
    stream << "  \"autoRefresh\": " << (settings.autoRefresh ? "true" : "false") << ",\n";
    stream << "  \"refreshIntervalMs\": " << settings.refreshIntervalMs << ",\n";
    stream << "  \"lastDllPath\": \"" << EscapeJson(WideToUtf8(settings.lastDllPath)) << "\",\n";
    stream << "  \"processFilter\": \"" << EscapeJson(WideToUtf8(settings.processFilter)) << "\",\n";
    stream << "  \"captureDllPath\": \"" << EscapeJson(WideToUtf8(settings.captureDllPath)) << "\",\n";
    stream << "  \"captureOutputDir\": \"" << EscapeJson(WideToUtf8(settings.captureOutputDir)) << "\",\n";
    stream << "  \"lastTextureAssetPath\": \"" << EscapeJson(WideToUtf8(settings.lastTextureAssetPath)) << "\",\n";
    stream << "  \"lastModelAssetPath\": \"" << EscapeJson(WideToUtf8(settings.lastModelAssetPath)) << "\",\n";
    stream << "  \"hookDllPath\": \"" << EscapeJson(WideToUtf8(settings.hookDllPath)) << "\",\n";
    stream << "  \"hookBackend\": \"" << EscapeJson(WideToUtf8(settings.hookBackend)) << "\"\n";
    stream << "}\n";

    return stream.good();
}

} // namespace rf::core
