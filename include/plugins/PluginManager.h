#pragma once

#include <Windows.h>

#include <string>
#include <vector>

#include "plugins/PluginApi.h"

namespace rf::plugins {

struct LoadedPlugin {
    std::wstring filePath;
    std::string name;
    HMODULE module = nullptr;
    PluginOnUnloadFn onUnload = nullptr;
};

class PluginManager {
public:
    ~PluginManager();

    void Reload(const std::vector<std::wstring>& pluginDirectories);
    void Reload(const std::wstring& pluginDirectory);
    void UnloadAll();

    const std::vector<LoadedPlugin>& Plugins() const;

private:
    std::vector<LoadedPlugin> loaded_;
};

} // namespace rf::plugins
