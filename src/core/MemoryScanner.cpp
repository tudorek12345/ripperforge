#include "core/MemoryScanner.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace rf::core {

namespace {

bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) {
        return false;
    }

    const DWORD basicProtection = protection & 0xFF;
    switch (basicProtection) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool MatchesPattern(const uint8_t* data, const std::vector<int>& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] >= 0 && data[i] != static_cast<uint8_t>(pattern[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ReadMemory(DWORD pid, uintptr_t address, size_t size, std::vector<uint8_t>& out, std::string& error) {
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        error = "OpenProcess failed.";
        return false;
    }

    out.assign(size, 0);
    SIZE_T bytesRead = 0;
    const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out.data(), size, &bytesRead);

    CloseHandle(process);

    if (!ok) {
        error = "ReadProcessMemory failed.";
        out.clear();
        return false;
    }

    out.resize(bytesRead);
    return true;
}

bool WriteMemory(DWORD pid, uintptr_t address, const std::vector<uint8_t>& data, std::string& error) {
    HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (process == nullptr) {
        error = "OpenProcess failed.";
        return false;
    }

    SIZE_T bytesWritten = 0;
    const BOOL ok = WriteProcessMemory(
        process,
        reinterpret_cast<LPVOID>(address),
        data.data(),
        data.size(),
        &bytesWritten);

    CloseHandle(process);

    if (!ok || bytesWritten != data.size()) {
        error = "WriteProcessMemory failed.";
        return false;
    }

    return true;
}

bool ParsePattern(const std::string& pattern, std::vector<int>& outPattern, std::string& error) {
    outPattern.clear();

    std::istringstream stream(pattern);
    std::string token;
    while (stream >> token) {
        if (token == "??" || token == "?") {
            outPattern.push_back(-1);
            continue;
        }

        if (token.size() != 2 || !std::isxdigit(static_cast<unsigned char>(token[0])) ||
            !std::isxdigit(static_cast<unsigned char>(token[1]))) {
            error = "Invalid pattern token: " + token;
            outPattern.clear();
            return false;
        }

        const int value = std::stoi(token, nullptr, 16);
        outPattern.push_back(value & 0xFF);
    }

    if (outPattern.empty()) {
        error = "Pattern cannot be empty.";
        return false;
    }

    return true;
}

std::vector<uintptr_t> ScanPattern(
    DWORD pid,
    uintptr_t start,
    uintptr_t end,
    const std::vector<int>& pattern,
    size_t maxResults,
    std::string& error,
    const std::atomic_bool* cancelRequested) {

    std::vector<uintptr_t> results;
    const auto isCanceled = [&]() {
        return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
    };

    if (isCanceled()) {
        error = "Scan canceled.";
        return results;
    }

    if (pattern.empty()) {
        error = "Pattern is empty.";
        return results;
    }

    if (end <= start) {
        error = "Invalid scan range.";
        return results;
    }

    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        error = "OpenProcess failed.";
        return results;
    }

    constexpr size_t kChunkSize = 1024 * 1024;
    const size_t overlap = pattern.size() > 1 ? pattern.size() - 1 : 0;

    uintptr_t cursor = start;
    while (cursor < end && results.size() < maxResults) {
        if (isCanceled()) {
            error = "Scan canceled.";
            break;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            break;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t nextCursor = regionBase + mbi.RegionSize;
        if (nextCursor <= cursor) {
            break;
        }

        if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect)) {
            const uintptr_t regionStart = std::max(start, regionBase);
            const uintptr_t regionEnd = std::min(end, nextCursor);

            std::vector<uint8_t> carry;
            uintptr_t regionCursor = regionStart;

            while (regionCursor < regionEnd && results.size() < maxResults) {
                if (isCanceled()) {
                    error = "Scan canceled.";
                    break;
                }

                const size_t toRead = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, regionEnd - regionCursor));
                std::vector<uint8_t> chunk(toRead);
                SIZE_T bytesRead = 0;

                if (!ReadProcessMemory(
                        process,
                        reinterpret_cast<LPCVOID>(regionCursor),
                        chunk.data(),
                        chunk.size(),
                        &bytesRead) ||
                    bytesRead == 0) {
                    carry.clear();
                    regionCursor += toRead;
                    continue;
                }

                chunk.resize(bytesRead);

                std::vector<uint8_t> data;
                data.reserve(carry.size() + chunk.size());
                data.insert(data.end(), carry.begin(), carry.end());
                data.insert(data.end(), chunk.begin(), chunk.end());

                const uintptr_t dataBase = regionCursor - carry.size();
                if (data.size() >= pattern.size()) {
                    for (size_t i = 0; i + pattern.size() <= data.size(); ++i) {
                        if (!MatchesPattern(data.data() + i, pattern)) {
                            continue;
                        }

                        results.push_back(dataBase + i);
                        if (results.size() >= maxResults) {
                            break;
                        }
                    }
                }

                if (overlap == 0) {
                    carry.clear();
                } else if (data.size() > overlap) {
                    carry.assign(data.end() - overlap, data.end());
                } else {
                    carry = data;
                }

                regionCursor += toRead;
            }
        }

        cursor = nextCursor;
    }

    CloseHandle(process);
    return results;
}

} // namespace rf::core
