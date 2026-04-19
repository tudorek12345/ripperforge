#include "core/MemoryScanner.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

template <typename T>
std::vector<uint8_t> ToBytes(T value) {
    std::vector<uint8_t> bytes(sizeof(T), 0);
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T>
bool FromBytes(const std::vector<uint8_t>& bytes, T& out) {
    if (bytes.size() != sizeof(T)) {
        return false;
    }

    std::memcpy(&out, bytes.data(), sizeof(T));
    return true;
}

bool ParseByteArray(const std::string& input, std::vector<uint8_t>& outBytes, std::string& error) {
    outBytes.clear();

    std::istringstream stream(input);
    std::string token;
    while (stream >> token) {
        if (token.size() > 2) {
            error = "Invalid byte token length: " + token;
            outBytes.clear();
            return false;
        }

        unsigned int value = 0;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (ec != std::errc() || ptr != token.data() + token.size() || value > 0xFF) {
            error = "Invalid byte token: " + token;
            outBytes.clear();
            return false;
        }

        outBytes.push_back(static_cast<uint8_t>(value));
    }

    if (outBytes.empty()) {
        error = "Byte array cannot be empty.";
        return false;
    }

    return true;
}

bool MatchesExactBytes(const uint8_t* data, const std::vector<uint8_t>& query) {
    if (query.empty()) {
        return false;
    }

    return std::memcmp(data, query.data(), query.size()) == 0;
}

std::string ByteArrayToString(const std::vector<uint8_t>& bytes) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');

    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }

    return stream.str();
}

template <typename T>
std::optional<int> CompareNumeric(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    T av{};
    T bv{};
    if (!FromBytes(a, av) || !FromBytes(b, bv)) {
        return std::nullopt;
    }

    if (av < bv) {
        return -1;
    }
    if (av > bv) {
        return 1;
    }
    return 0;
}

std::optional<int> CompareTypedValues(
    TypedScanValueType type,
    const std::vector<uint8_t>& current,
    const std::vector<uint8_t>& previous) {

    switch (type) {
    case TypedScanValueType::Int32:
        return CompareNumeric<int32_t>(current, previous);
    case TypedScanValueType::Int64:
        return CompareNumeric<int64_t>(current, previous);
    case TypedScanValueType::Float:
        return CompareNumeric<float>(current, previous);
    case TypedScanValueType::Double:
        return CompareNumeric<double>(current, previous);
    default:
        return std::nullopt;
    }
}

bool IsNumericType(TypedScanValueType type) {
    return type == TypedScanValueType::Int32 || type == TypedScanValueType::Int64 ||
           type == TypedScanValueType::Float || type == TypedScanValueType::Double;
}

bool ReadProcessMemoryExact(HANDLE process, uintptr_t address, size_t size, std::vector<uint8_t>& outBytes) {
    outBytes.assign(size, 0);
    SIZE_T bytesRead = 0;
    const BOOL ok = ReadProcessMemory(
        process,
        reinterpret_cast<LPCVOID>(address),
        outBytes.data(),
        size,
        &bytesRead);
    if (!ok || bytesRead != size) {
        outBytes.clear();
        return false;
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

bool ParseTypedValueInput(
    TypedScanValueType type,
    const std::string& input,
    std::vector<uint8_t>& outBytes,
    std::string& error) {

    outBytes.clear();

    try {
        switch (type) {
        case TypedScanValueType::Int32: {
            const long long value = std::stoll(input);
            if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
                error = "Value out of range for int32.";
                return false;
            }
            outBytes = ToBytes<int32_t>(static_cast<int32_t>(value));
            return true;
        }
        case TypedScanValueType::Int64: {
            const long long value = std::stoll(input);
            outBytes = ToBytes<int64_t>(static_cast<int64_t>(value));
            return true;
        }
        case TypedScanValueType::Float: {
            const float value = std::stof(input);
            outBytes = ToBytes<float>(value);
            return true;
        }
        case TypedScanValueType::Double: {
            const double value = std::stod(input);
            outBytes = ToBytes<double>(value);
            return true;
        }
        case TypedScanValueType::Utf8String: {
            if (input.empty()) {
                error = "String value cannot be empty.";
                return false;
            }
            outBytes.assign(input.begin(), input.end());
            return true;
        }
        case TypedScanValueType::ByteArray:
            return ParseByteArray(input, outBytes, error);
        default:
            error = "Unsupported typed scan value type.";
            return false;
        }
    } catch (...) {
        error = "Invalid value format for selected type.";
        outBytes.clear();
        return false;
    }
}

bool TypedValueBytesToString(
    TypedScanValueType type,
    const std::vector<uint8_t>& bytes,
    std::string& out,
    std::string& error) {

    out.clear();
    error.clear();

    std::ostringstream stream;
    stream << std::setprecision(8);

    switch (type) {
    case TypedScanValueType::Int32: {
        int32_t value = 0;
        if (!FromBytes(bytes, value)) {
            error = "Invalid int32 byte payload.";
            return false;
        }
        out = std::to_string(value);
        return true;
    }
    case TypedScanValueType::Int64: {
        int64_t value = 0;
        if (!FromBytes(bytes, value)) {
            error = "Invalid int64 byte payload.";
            return false;
        }
        out = std::to_string(value);
        return true;
    }
    case TypedScanValueType::Float: {
        float value = 0.0f;
        if (!FromBytes(bytes, value)) {
            error = "Invalid float byte payload.";
            return false;
        }
        stream << value;
        out = stream.str();
        return true;
    }
    case TypedScanValueType::Double: {
        double value = 0.0;
        if (!FromBytes(bytes, value)) {
            error = "Invalid double byte payload.";
            return false;
        }
        stream << value;
        out = stream.str();
        return true;
    }
    case TypedScanValueType::Utf8String:
        out.assign(bytes.begin(), bytes.end());
        return true;
    case TypedScanValueType::ByteArray:
        out = ByteArrayToString(bytes);
        return true;
    default:
        error = "Unsupported typed value type.";
        return false;
    }
}

bool FirstTypedScan(
    DWORD pid,
    uintptr_t start,
    uintptr_t end,
    TypedScanValueType type,
    const std::vector<uint8_t>& queryValue,
    size_t maxResults,
    TypedScanSession& outSession,
    std::string& error,
    const std::atomic_bool* cancelRequested) {

    outSession = {};
    const auto isCanceled = [&]() {
        return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
    };

    if (isCanceled()) {
        error = "Scan canceled.";
        return false;
    }

    if (pid == 0) {
        error = "Invalid target process.";
        return false;
    }
    if (end <= start) {
        error = "Invalid scan range.";
        return false;
    }
    if (queryValue.empty()) {
        error = "Typed scan query value is empty.";
        return false;
    }
    if (maxResults == 0) {
        error = "Max results must be greater than zero.";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        error = "OpenProcess failed.";
        return false;
    }

    constexpr size_t kChunkSize = 1024 * 1024;
    const size_t overlap = queryValue.size() > 1 ? queryValue.size() - 1 : 0;

    uintptr_t cursor = start;
    while (cursor < end && outSession.addresses.size() < maxResults) {
        if (isCanceled()) {
            error = "Scan canceled.";
            break;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            break;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t nextCursor = regionBase + mbi.RegionSize;
        if (nextCursor <= cursor) {
            break;
        }

        if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect)) {
            const uintptr_t regionStart = std::max(start, regionBase);
            const uintptr_t regionEnd = std::min(end, nextCursor);

            std::vector<uint8_t> carry;
            uintptr_t regionCursor = regionStart;
            while (regionCursor < regionEnd && outSession.addresses.size() < maxResults) {
                if (isCanceled()) {
                    error = "Scan canceled.";
                    break;
                }

                const size_t toRead = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, regionEnd - regionCursor));
                std::vector<uint8_t> chunk(toRead, 0);
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
                if (data.size() >= queryValue.size()) {
                    for (size_t i = 0; i + queryValue.size() <= data.size(); ++i) {
                        if (!MatchesExactBytes(data.data() + i, queryValue)) {
                            continue;
                        }

                        outSession.addresses.push_back(dataBase + i);
                        outSession.snapshots.emplace_back(data.begin() + static_cast<ptrdiff_t>(i),
                                                          data.begin() + static_cast<ptrdiff_t>(i + queryValue.size()));
                        if (outSession.addresses.size() >= maxResults) {
                            break;
                        }
                    }
                }

                if (overlap == 0) {
                    carry.clear();
                } else if (data.size() > overlap) {
                    carry.assign(data.end() - static_cast<ptrdiff_t>(overlap), data.end());
                } else {
                    carry = data;
                }

                regionCursor += toRead;
            }
        }

        cursor = nextCursor;
    }

    CloseHandle(process);

    outSession.pid = pid;
    outSession.type = type;
    outSession.queryValue = queryValue;
    return error.empty() || error == "Scan canceled.";
}

bool NextTypedScan(
    const TypedScanSession& previous,
    TypedScanCompareMode mode,
    const std::vector<uint8_t>& equalsValue,
    size_t maxResults,
    TypedScanSession& outSession,
    std::string& error,
    const std::atomic_bool* cancelRequested) {

    outSession = {};
    const auto isCanceled = [&]() {
        return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
    };

    if (isCanceled()) {
        error = "Scan canceled.";
        return false;
    }

    if (previous.pid == 0 || previous.addresses.empty() || previous.queryValue.empty()) {
        error = "No previous scan session available.";
        return false;
    }
    if (maxResults == 0) {
        error = "Max results must be greater than zero.";
        return false;
    }

    const size_t valueSize = previous.queryValue.size();
    if (mode == TypedScanCompareMode::Equals && equalsValue.size() != valueSize) {
        error = "Equals value size does not match active scan type.";
        return false;
    }

    const bool needsSnapshot =
        mode == TypedScanCompareMode::Changed || mode == TypedScanCompareMode::Unchanged ||
        mode == TypedScanCompareMode::Increased || mode == TypedScanCompareMode::Decreased;

    if (needsSnapshot && previous.snapshots.size() != previous.addresses.size()) {
        error = "Previous scan snapshots are missing.";
        return false;
    }

    if ((mode == TypedScanCompareMode::Increased || mode == TypedScanCompareMode::Decreased) &&
        !IsNumericType(previous.type)) {
        error = "Increased/decreased filters require numeric scan types.";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, previous.pid);
    if (process == nullptr) {
        error = "OpenProcess failed.";
        return false;
    }

    for (size_t i = 0; i < previous.addresses.size() && outSession.addresses.size() < maxResults; ++i) {
        if (isCanceled()) {
            error = "Scan canceled.";
            break;
        }

        const uintptr_t address = previous.addresses[i];
        std::vector<uint8_t> currentBytes;
        if (!ReadProcessMemoryExact(process, address, valueSize, currentBytes)) {
            continue;
        }

        bool keep = false;
        switch (mode) {
        case TypedScanCompareMode::Exact:
            keep = (currentBytes == previous.queryValue);
            break;
        case TypedScanCompareMode::Equals:
            keep = (currentBytes == equalsValue);
            break;
        case TypedScanCompareMode::Changed:
            keep = (currentBytes != previous.snapshots[i]);
            break;
        case TypedScanCompareMode::Unchanged:
            keep = (currentBytes == previous.snapshots[i]);
            break;
        case TypedScanCompareMode::Increased:
        case TypedScanCompareMode::Decreased: {
            const std::optional<int> cmp = CompareTypedValues(previous.type, currentBytes, previous.snapshots[i]);
            if (!cmp.has_value()) {
                keep = false;
            } else if (mode == TypedScanCompareMode::Increased) {
                keep = (*cmp > 0);
            } else {
                keep = (*cmp < 0);
            }
            break;
        }
        default:
            keep = false;
            break;
        }

        if (keep) {
            outSession.addresses.push_back(address);
            outSession.snapshots.push_back(std::move(currentBytes));
        }
    }

    CloseHandle(process);

    outSession.pid = previous.pid;
    outSession.type = previous.type;
    outSession.queryValue = previous.queryValue;
    return error.empty() || error == "Scan canceled.";
}

bool ResolvePointerChain(
    DWORD pid,
    uintptr_t baseAddress,
    const std::vector<uintptr_t>& offsets,
    uintptr_t& outFinalAddress,
    std::string& error) {

    outFinalAddress = 0;
    if (pid == 0) {
        error = "Invalid target process.";
        return false;
    }
    if (baseAddress == 0) {
        error = "Base address cannot be zero.";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        error = "OpenProcess failed.";
        return false;
    }

    uintptr_t current = baseAddress;
    for (size_t i = 0; i < offsets.size(); ++i) {
        uintptr_t pointerValue = 0;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                process,
                reinterpret_cast<LPCVOID>(current),
                &pointerValue,
                sizeof(pointerValue),
                &bytesRead) ||
            bytesRead != sizeof(pointerValue)) {
            CloseHandle(process);
            error = "Pointer read failed at chain index " + std::to_string(i) + ".";
            return false;
        }

        current = pointerValue + offsets[i];
    }

    uint8_t probe = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(current),
            &probe,
            sizeof(probe),
            &bytesRead) ||
        bytesRead != sizeof(probe)) {
        CloseHandle(process);
        error = "Resolved target address is unreadable.";
        return false;
    }

    CloseHandle(process);
    outFinalAddress = current;
    return true;
}

} // namespace rf::core
