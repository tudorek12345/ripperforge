#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rf::core {

struct ReverseToolkitScanDefaults {
    std::wstring valueType = L"int32";
    std::wstring compareMode = L"exact";
    std::wstring valueInput;
    std::wstring rangeStartHex;
    std::wstring rangeEndHex;
};

struct ReverseToolkitWatchEntry {
    std::wstring label;
    uint64_t address = 0;
    std::wstring valueType = L"int32";
    std::wstring freezeValue;
    bool freeze = false;
};

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

    std::wstring uiLayout = L"dockspace-default";
    std::wstring uiTheme = L"industrial-dark";
    std::wstring uiDensity = L"compact";

    std::wstring runtimeDataRoot;
    std::wstring runtimeUserPluginDir;
    std::wstring runtimeBundledPluginDir;
    std::wstring runtimeBundledCaptureDllPath;
    bool legacyConfigMigrated = false;

    ReverseToolkitScanDefaults reverseToolkitScanDefaults{};
    std::vector<ReverseToolkitWatchEntry> reverseToolkitWatchList;
    uint32_t reverseToolkitFreezeIntervalMs = 120;
};

AppSettings LoadSettings(const std::wstring& filePath);
bool SaveSettings(const std::wstring& filePath, const AppSettings& settings);

} // namespace rf::core
