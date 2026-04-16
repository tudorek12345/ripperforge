#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace rf::core {

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
    std::wstring imagePath;
};

std::vector<ProcessInfo> EnumerateProcesses(const std::wstring& filter);
bool IsRunningAsAdmin();
bool RelaunchAsAdmin();

} // namespace rf::core
