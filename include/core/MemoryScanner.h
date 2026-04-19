#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rf::core {

enum class TypedScanValueType : uint8_t {
    Int32 = 0,
    Int64,
    Float,
    Double,
    Utf8String,
    ByteArray,
};

enum class TypedScanCompareMode : uint8_t {
    Exact = 0,
    Changed,
    Unchanged,
    Increased,
    Decreased,
    Equals,
};

struct TypedScanSession {
    DWORD pid = 0;
    TypedScanValueType type = TypedScanValueType::Int32;
    std::vector<uint8_t> queryValue;
    std::vector<uintptr_t> addresses;
    std::vector<std::vector<uint8_t>> snapshots;
};

bool ReadMemory(DWORD pid, uintptr_t address, size_t size, std::vector<uint8_t>& out, std::string& error);
bool WriteMemory(DWORD pid, uintptr_t address, const std::vector<uint8_t>& data, std::string& error);
bool ParsePattern(const std::string& pattern, std::vector<int>& outPattern, std::string& error);
std::vector<uintptr_t> ScanPattern(
    DWORD pid,
    uintptr_t start,
    uintptr_t end,
    const std::vector<int>& pattern,
    size_t maxResults,
    std::string& error,
    const std::atomic_bool* cancelRequested = nullptr);

bool ParseTypedValueInput(
    TypedScanValueType type,
    const std::string& input,
    std::vector<uint8_t>& outBytes,
    std::string& error);

bool TypedValueBytesToString(
    TypedScanValueType type,
    const std::vector<uint8_t>& bytes,
    std::string& out,
    std::string& error);

bool FirstTypedScan(
    DWORD pid,
    uintptr_t start,
    uintptr_t end,
    TypedScanValueType type,
    const std::vector<uint8_t>& queryValue,
    size_t maxResults,
    TypedScanSession& outSession,
    std::string& error,
    const std::atomic_bool* cancelRequested = nullptr);

bool NextTypedScan(
    const TypedScanSession& previous,
    TypedScanCompareMode mode,
    const std::vector<uint8_t>& equalsValue,
    size_t maxResults,
    TypedScanSession& outSession,
    std::string& error,
    const std::atomic_bool* cancelRequested = nullptr);

bool ResolvePointerChain(
    DWORD pid,
    uintptr_t baseAddress,
    const std::vector<uintptr_t>& offsets,
    uintptr_t& outFinalAddress,
    std::string& error);

} // namespace rf::core
