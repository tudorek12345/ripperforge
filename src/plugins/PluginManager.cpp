#include "plugins/PluginManager.h"

#include <filesystem>
#include <cwctype>
#include <unordered_set>

#include "core/Logger.h"

namespace rf::plugins {

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

void __stdcall PluginLogBridge(const char* message) {
    rf::core::Logger::Instance().Info(std::string("[Plugin] ") + (message != nullptr ? message : "(null)"));
}

std::wstring NormalizePathKey(std::wstring path) {
    for (auto& ch : path) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return path;
}

} // namespace

PluginManager::~PluginManager() {
    UnloadAll();
}

void PluginManager::Reload(const std::vector<std::wstring>& pluginDirectories) {
    UnloadAll();

    if (pluginDirectories.empty()) {
        rf::core::Logger::Instance().Info("Plugin scan skipped: no directories configured.");
        return;
    }

    struct CandidatePlugin {
        std::wstring filePath;
        size_t sourceIndex = 0;
    };

    std::vector<CandidatePlugin> candidates;
    std::vector<size_t> discoveredBySource(pluginDirectories.size(), 0);
    std::vector<size_t> loadedBySource(pluginDirectories.size(), 0);
    std::unordered_set<std::wstring> seenNormalizedPaths;

    for (size_t sourceIndex = 0; sourceIndex < pluginDirectories.size(); ++sourceIndex) {
        const std::wstring& pluginDirectory = pluginDirectories[sourceIndex];
        if (pluginDirectory.empty()) {
            continue;
        }

        std::error_code errorCode;
        if (!std::filesystem::exists(pluginDirectory, errorCode)) {
            std::filesystem::create_directories(pluginDirectory, errorCode);
            if (errorCode) {
                rf::core::Logger::Instance().Error(
                    "Failed to create plugin directory: " + WideToUtf8(pluginDirectory));
                continue;
            }
        }

        rf::core::Logger::Instance().Info("Plugin scan directory: " + WideToUtf8(pluginDirectory));

        for (const auto& entry : std::filesystem::directory_iterator(pluginDirectory, errorCode)) {
            if (errorCode) {
                rf::core::Logger::Instance().Error(
                    "Failed to enumerate plugin directory: " + WideToUtf8(pluginDirectory));
                break;
            }

            if (!entry.is_regular_file()) {
                continue;
            }

            if (entry.path().extension() != L".dll") {
                continue;
            }

            const std::filesystem::path candidatePath = entry.path().lexically_normal();
            const std::wstring normalizedPath = NormalizePathKey(candidatePath.wstring());
            if (!seenNormalizedPaths.insert(normalizedPath).second) {
                continue;
            }

            ++discoveredBySource[sourceIndex];
            CandidatePlugin candidate;
            candidate.filePath = candidatePath.wstring();
            candidate.sourceIndex = sourceIndex;
            candidates.push_back(std::move(candidate));
        }
    }

    const HostApi hostApi{PluginLogBridge};

    for (const auto& candidate : candidates) {
        HMODULE module = LoadLibraryW(candidate.filePath.c_str());
        if (module == nullptr) {
            rf::core::Logger::Instance().Error("Failed to load plugin: " + WideToUtf8(candidate.filePath));
            continue;
        }

        auto nameFn = reinterpret_cast<PluginNameFn>(GetProcAddress(module, "RF_PluginName"));
        auto onLoadFn = reinterpret_cast<PluginOnLoadFn>(GetProcAddress(module, "RF_OnLoad"));
        auto onUnloadFn = reinterpret_cast<PluginOnUnloadFn>(GetProcAddress(module, "RF_OnUnload"));

        if (nameFn == nullptr || onLoadFn == nullptr) {
            rf::core::Logger::Instance().Error("Plugin missing required exports: " + WideToUtf8(candidate.filePath));
            FreeLibrary(module);
            continue;
        }

        const bool loaded = onLoadFn(&hostApi);
        if (!loaded) {
            rf::core::Logger::Instance().Error("Plugin rejected load request: " + WideToUtf8(candidate.filePath));
            FreeLibrary(module);
            continue;
        }

        LoadedPlugin plugin;
        plugin.filePath = candidate.filePath;
        plugin.name = nameFn != nullptr ? nameFn() : "UnnamedPlugin";
        plugin.module = module;
        plugin.onUnload = onUnloadFn;

        ++loadedBySource[candidate.sourceIndex];
        rf::core::Logger::Instance().Info("Loaded plugin: " + plugin.name);
        loaded_.push_back(std::move(plugin));
    }

    for (size_t i = 0; i < pluginDirectories.size(); ++i) {
        rf::core::Logger::Instance().Info(
            "Plugin source summary: " + WideToUtf8(pluginDirectories[i]) +
            " discovered=" + std::to_string(discoveredBySource[i]) +
            ", loaded=" + std::to_string(loadedBySource[i]) + ".");
    }
}

void PluginManager::Reload(const std::wstring& pluginDirectory) {
    Reload(std::vector<std::wstring>{pluginDirectory});
}

void PluginManager::UnloadAll() {
    for (auto& plugin : loaded_) {
        if (plugin.onUnload != nullptr) {
            plugin.onUnload();
        }

        if (plugin.module != nullptr) {
            FreeLibrary(plugin.module);
            plugin.module = nullptr;
        }
    }

    loaded_.clear();
}

const std::vector<LoadedPlugin>& PluginManager::Plugins() const {
    return loaded_;
}

} // namespace rf::plugins
