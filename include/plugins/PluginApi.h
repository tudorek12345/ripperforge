#pragma once

#include <Windows.h>

namespace rf::plugins {

struct HostApi {
    void(__stdcall* Log)(const char* message);
};

using PluginNameFn = const char*(__stdcall*)();
using PluginOnLoadFn = bool(__stdcall*)(const HostApi* hostApi);
using PluginOnUnloadFn = void(__stdcall*)();

} // namespace rf::plugins
