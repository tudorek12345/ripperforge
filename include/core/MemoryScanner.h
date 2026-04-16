#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rf::core {

bool ReadMemory(DWORD pid, uintptr_t address, size_t size, std::vector<uint8_t>& out, std::string& error);
bool WriteMemory(DWORD pid, uintptr_t address, const std::vector<uint8_t>& data, std::string& error);
bool ParsePattern(const std::string& pattern, std::vector<int>& outPattern, std::string& error);
std::vector<uintptr_t> ScanPattern(
    DWORD pid,
    uintptr_t start,
    uintptr_t end,
    const std::vector<int>& pattern,
    size_t maxResults,
    std::string& error);

} // namespace rf::core
