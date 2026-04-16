#include "plugins/PluginManager.h"

#include <filesystem>

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

} // namespace

PluginManager::~PluginManager() {
    UnloadAll();
}

void PluginManager::Reload(const std::wstring& pluginDirectory) {
    UnloadAll();

    std::error_code errorCode;
    if (!std::filesystem::exists(pluginDirectory, errorCode)) {
        std::filesystem::create_directories(pluginDirectory, errorCode);
        if (errorCode) {
            rf::core::Logger::Instance().Error("Failed to create plugin directory.");
            return;
        }
    }

    const HostApi hostApi{PluginLogBridge};

    for (const auto& entry : std::filesystem::directory_iterator(pluginDirectory, errorCode)) {
        if (errorCode) {
            rf::core::Logger::Instance().Error("Failed to enumerate plugin directory.");
            break;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() != L".dll") {
            continue;
        }

        const std::wstring filePath = entry.path().wstring();
        HMODULE module = LoadLibraryW(filePath.c_str());
        if (module == nullptr) {
            rf::core::Logger::Instance().Error("Failed to load plugin: " + WideToUtf8(filePath));
            continue;
        }

        auto nameFn = reinterpret_cast<PluginNameFn>(GetProcAddress(module, "RF_PluginName"));
        auto onLoadFn = reinterpret_cast<PluginOnLoadFn>(GetProcAddress(module, "RF_OnLoad"));
        auto onUnloadFn = reinterpret_cast<PluginOnUnloadFn>(GetProcAddress(module, "RF_OnUnload"));

        if (nameFn == nullptr || onLoadFn == nullptr) {
            rf::core::Logger::Instance().Error("Plugin missing required exports: " + WideToUtf8(filePath));
            FreeLibrary(module);
            continue;
        }

        const bool loaded = onLoadFn(&hostApi);
        if (!loaded) {
            rf::core::Logger::Instance().Error("Plugin rejected load request: " + WideToUtf8(filePath));
            FreeLibrary(module);
            continue;
        }

        LoadedPlugin plugin;
        plugin.filePath = filePath;
        plugin.name = nameFn != nullptr ? nameFn() : "UnnamedPlugin";
        plugin.module = module;
        plugin.onUnload = onUnloadFn;

        rf::core::Logger::Instance().Info("Loaded plugin: " + plugin.name);
        loaded_.push_back(std::move(plugin));
    }
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
