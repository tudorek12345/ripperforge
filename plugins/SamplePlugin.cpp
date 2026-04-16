#include <string>

#include "plugins/PluginApi.h"

static const rf::plugins::HostApi* g_host = nullptr;

extern "C" __declspec(dllexport) const char* __stdcall RF_PluginName() {
    return "SampleRipperPlugin";
}

extern "C" __declspec(dllexport) bool __stdcall RF_OnLoad(const rf::plugins::HostApi* hostApi) {
    g_host = hostApi;
    if (g_host != nullptr && g_host->Log != nullptr) {
        g_host->Log("SampleRipperPlugin loaded.");
    }
    return true;
}

extern "C" __declspec(dllexport) void __stdcall RF_OnUnload() {
    if (g_host != nullptr && g_host->Log != nullptr) {
        g_host->Log("SampleRipperPlugin unloaded.");
    }
    g_host = nullptr;
}
