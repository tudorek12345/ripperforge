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

std::optional<bool> ParseBool(const std::string& input, const std::regex& expression) {
    if (auto value = MatchValue(input, expression)) {
        return *value == "true";
    }
    return std::nullopt;
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

    static const std::regex autoRefreshRegex(R"json("autoRefresh"\s*:\s*(true|false))json");
    static const std::regex intervalRegex(R"json("refreshIntervalMs"\s*:\s*(\d+))json");
    static const std::regex lastDllRegex(R"json("lastDllPath"\s*:\s*"([^"]*)")json");
    static const std::regex filterRegex(R"json("processFilter"\s*:\s*"([^"]*)")json");
    static const std::regex captureDllRegex(R"json("captureDllPath"\s*:\s*"([^"]*)")json");
    static const std::regex captureOutputRegex(R"json("captureOutputDir"\s*:\s*"([^"]*)")json");
    static const std::regex lastTextureRegex(R"json("lastTextureAssetPath"\s*:\s*"([^"]*)")json");
    static const std::regex lastModelRegex(R"json("lastModelAssetPath"\s*:\s*"([^"]*)")json");
    static const std::regex hookDllRegex(R"json("hookDllPath"\s*:\s*"([^"]*)")json");
    static const std::regex hookBackendRegex(R"json("hookBackend"\s*:\s*"([^"]*)")json");

    static const std::regex uiLayoutRegex(R"json("ui\.layout"\s*:\s*"([^"]*)")json");
    static const std::regex uiThemeRegex(R"json("ui\.theme"\s*:\s*"([^"]*)")json");
    static const std::regex uiDensityRegex(R"json("ui\.density"\s*:\s*"([^"]*)")json");
    static const std::regex runtimeDataRootRegex(R"json("runtime\.dataRoot"\s*:\s*"([^"]*)")json");
    static const std::regex runtimeUserPluginDirRegex(R"json("runtime\.userPluginDir"\s*:\s*"([^"]*)")json");
    static const std::regex runtimeBundledPluginDirRegex(R"json("runtime\.bundledPluginDir"\s*:\s*"([^"]*)")json");
    static const std::regex runtimeBundledCaptureDllRegex(R"json("runtime\.bundledCaptureDllPath"\s*:\s*"([^"]*)")json");
    static const std::regex runtimeLegacyMigratedRegex(R"json("runtime\.legacyConfigMigrated"\s*:\s*(true|false))json");

    static const std::regex freezeIntervalRegex(R"json("reverseToolkit\.freezeIntervalMs"\s*:\s*(\d+))json");
    static const std::regex scanDefaultsBlockRegex(R"json("reverseToolkit\.scanDefaults"\s*:\s*\{([\s\S]*?)\})json");
    static const std::regex watchListBlockRegex(R"json("reverseToolkit\.watchList"\s*:\s*\[([\s\S]*?)\])json");

    static const std::regex scanValueTypeRegex(R"json("valueType"\s*:\s*"([^"]*)")json");
    static const std::regex scanCompareModeRegex(R"json("compareMode"\s*:\s*"([^"]*)")json");
    static const std::regex scanValueInputRegex(R"json("valueInput"\s*:\s*"([^"]*)")json");
    static const std::regex scanRangeStartRegex(R"json("rangeStartHex"\s*:\s*"([^"]*)")json");
    static const std::regex scanRangeEndRegex(R"json("rangeEndHex"\s*:\s*"([^"]*)")json");

    static const std::regex watchEntryRegex(R"json(\{([\s\S]*?)\})json");
    static const std::regex watchLabelRegex(R"json("label"\s*:\s*"([^"]*)")json");
    static const std::regex watchAddressRegex(R"json("address"\s*:\s*(\d+))json");
    static const std::regex watchValueTypeRegex(R"json("valueType"\s*:\s*"([^"]*)")json");
    static const std::regex watchFreezeValueRegex(R"json("freezeValue"\s*:\s*"([^"]*)")json");
    static const std::regex watchFreezeRegex(R"json("freeze"\s*:\s*(true|false))json");

    if (auto value = ParseBool(content, autoRefreshRegex)) {
        settings.autoRefresh = *value;
    }

    if (auto intervalMatch = MatchValue(content, intervalRegex)) {
        settings.refreshIntervalMs = static_cast<uint32_t>(std::stoul(*intervalMatch));
    }

    if (auto value = MatchValue(content, lastDllRegex)) {
        settings.lastDllPath = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, filterRegex)) {
        settings.processFilter = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, captureDllRegex)) {
        settings.captureDllPath = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, captureOutputRegex)) {
        settings.captureOutputDir = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, lastTextureRegex)) {
        settings.lastTextureAssetPath = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, lastModelRegex)) {
        settings.lastModelAssetPath = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, hookDllRegex)) {
        settings.hookDllPath = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, hookBackendRegex)) {
        settings.hookBackend = Utf8ToWide(UnescapeJson(*value));
    }

    if (auto value = MatchValue(content, uiLayoutRegex)) {
        settings.uiLayout = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, uiThemeRegex)) {
        settings.uiTheme = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, uiDensityRegex)) {
        settings.uiDensity = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, runtimeDataRootRegex)) {
        settings.runtimeDataRoot = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, runtimeUserPluginDirRegex)) {
        settings.runtimeUserPluginDir = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, runtimeBundledPluginDirRegex)) {
        settings.runtimeBundledPluginDir = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = MatchValue(content, runtimeBundledCaptureDllRegex)) {
        settings.runtimeBundledCaptureDllPath = Utf8ToWide(UnescapeJson(*value));
    }
    if (auto value = ParseBool(content, runtimeLegacyMigratedRegex)) {
        settings.legacyConfigMigrated = *value;
    }

    if (auto value = MatchValue(content, freezeIntervalRegex)) {
        settings.reverseToolkitFreezeIntervalMs = static_cast<uint32_t>(std::stoul(*value));
    }

    if (auto scanBlock = MatchValue(content, scanDefaultsBlockRegex)) {
        if (auto value = MatchValue(*scanBlock, scanValueTypeRegex)) {
            settings.reverseToolkitScanDefaults.valueType = Utf8ToWide(UnescapeJson(*value));
        }
        if (auto value = MatchValue(*scanBlock, scanCompareModeRegex)) {
            settings.reverseToolkitScanDefaults.compareMode = Utf8ToWide(UnescapeJson(*value));
        }
        if (auto value = MatchValue(*scanBlock, scanValueInputRegex)) {
            settings.reverseToolkitScanDefaults.valueInput = Utf8ToWide(UnescapeJson(*value));
        }
        if (auto value = MatchValue(*scanBlock, scanRangeStartRegex)) {
            settings.reverseToolkitScanDefaults.rangeStartHex = Utf8ToWide(UnescapeJson(*value));
        }
        if (auto value = MatchValue(*scanBlock, scanRangeEndRegex)) {
            settings.reverseToolkitScanDefaults.rangeEndHex = Utf8ToWide(UnescapeJson(*value));
        }
    }

    if (auto watchBlock = MatchValue(content, watchListBlockRegex)) {
        settings.reverseToolkitWatchList.clear();

        auto begin = std::sregex_iterator(watchBlock->begin(), watchBlock->end(), watchEntryRegex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::string objectBody = (*it)[1].str();
            ReverseToolkitWatchEntry entry;

            if (auto value = MatchValue(objectBody, watchLabelRegex)) {
                entry.label = Utf8ToWide(UnescapeJson(*value));
            }
            if (auto value = MatchValue(objectBody, watchAddressRegex)) {
                entry.address = std::stoull(*value);
            }
            if (auto value = MatchValue(objectBody, watchValueTypeRegex)) {
                entry.valueType = Utf8ToWide(UnescapeJson(*value));
            }
            if (auto value = MatchValue(objectBody, watchFreezeValueRegex)) {
                entry.freezeValue = Utf8ToWide(UnescapeJson(*value));
            }
            if (auto value = ParseBool(objectBody, watchFreezeRegex)) {
                entry.freeze = *value;
            }

            if (entry.address != 0) {
                settings.reverseToolkitWatchList.push_back(std::move(entry));
            }
        }
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
    stream << "  \"hookBackend\": \"" << EscapeJson(WideToUtf8(settings.hookBackend)) << "\",\n";

    stream << "  \"ui.layout\": \"" << EscapeJson(WideToUtf8(settings.uiLayout)) << "\",\n";
    stream << "  \"ui.theme\": \"" << EscapeJson(WideToUtf8(settings.uiTheme)) << "\",\n";
    stream << "  \"ui.density\": \"" << EscapeJson(WideToUtf8(settings.uiDensity)) << "\",\n";
    stream << "  \"runtime.dataRoot\": \"" << EscapeJson(WideToUtf8(settings.runtimeDataRoot)) << "\",\n";
    stream << "  \"runtime.userPluginDir\": \"" << EscapeJson(WideToUtf8(settings.runtimeUserPluginDir)) << "\",\n";
    stream << "  \"runtime.bundledPluginDir\": \"" << EscapeJson(WideToUtf8(settings.runtimeBundledPluginDir)) << "\",\n";
    stream << "  \"runtime.bundledCaptureDllPath\": \"" << EscapeJson(WideToUtf8(settings.runtimeBundledCaptureDllPath)) << "\",\n";
    stream << "  \"runtime.legacyConfigMigrated\": " << (settings.legacyConfigMigrated ? "true" : "false") << ",\n";

    stream << "  \"reverseToolkit.scanDefaults\": {\n";
    stream << "    \"valueType\": \"" << EscapeJson(WideToUtf8(settings.reverseToolkitScanDefaults.valueType)) << "\",\n";
    stream << "    \"compareMode\": \"" << EscapeJson(WideToUtf8(settings.reverseToolkitScanDefaults.compareMode)) << "\",\n";
    stream << "    \"valueInput\": \"" << EscapeJson(WideToUtf8(settings.reverseToolkitScanDefaults.valueInput)) << "\",\n";
    stream << "    \"rangeStartHex\": \"" << EscapeJson(WideToUtf8(settings.reverseToolkitScanDefaults.rangeStartHex)) << "\",\n";
    stream << "    \"rangeEndHex\": \"" << EscapeJson(WideToUtf8(settings.reverseToolkitScanDefaults.rangeEndHex)) << "\"\n";
    stream << "  },\n";

    stream << "  \"reverseToolkit.watchList\": [\n";
    for (size_t i = 0; i < settings.reverseToolkitWatchList.size(); ++i) {
        const auto& entry = settings.reverseToolkitWatchList[i];
        stream << "    {\"label\": \"" << EscapeJson(WideToUtf8(entry.label))
               << "\", \"address\": " << entry.address
               << ", \"valueType\": \"" << EscapeJson(WideToUtf8(entry.valueType))
               << "\", \"freezeValue\": \"" << EscapeJson(WideToUtf8(entry.freezeValue))
               << "\", \"freeze\": " << (entry.freeze ? "true" : "false") << "}";
        if (i + 1 < settings.reverseToolkitWatchList.size()) {
            stream << ",";
        }
        stream << "\n";
    }
    stream << "  ],\n";
    stream << "  \"reverseToolkit.freezeIntervalMs\": " << settings.reverseToolkitFreezeIntervalMs << "\n";
    stream << "}\n";

    return stream.good();
}

} // namespace rf::core
