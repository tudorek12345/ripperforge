#pragma once

#include <cstdint>
#include <string>

namespace rf::core {

struct AppSettings {
    bool autoRefresh = true;
    uint32_t refreshIntervalMs = 2000;
    std::wstring lastDllPath;
    std::wstring processFilter;
    std::wstring captureDllPath;
    std::wstring captureOutputDir;
    std::wstring lastTextureAssetPath;
    std::wstring lastModelAssetPath;
    std::wstring hookDllPath;
    std::wstring hookBackend = L"MinHook";
};

AppSettings LoadSettings(const std::wstring& filePath);
bool SaveSettings(const std::wstring& filePath, const AppSettings& settings);

} // namespace rf::core
