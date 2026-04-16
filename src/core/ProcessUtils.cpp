#include "core/ProcessUtils.h"

#include <TlHelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>

namespace rf::core {

namespace {

std::wstring ToLower(const std::wstring& value) {
    std::wstring lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return lowered;
}

bool ContainsInsensitive(const std::wstring& text, const std::wstring& filter) {
    if (filter.empty()) {
        return true;
    }

    return ToLower(text).find(ToLower(filter)) != std::wstring::npos;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const std::wstring name = std::filesystem::path(path).filename().wstring();
    if (name.empty()) {
        return path;
    }
    return name;
}

} // namespace

std::vector<ProcessInfo> EnumerateProcesses(const std::wstring& filter) {
    std::vector<ProcessInfo> processes;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return processes;
    }

    do {
        ProcessInfo info;
        info.pid = entry.th32ProcessID;
        info.name = entry.szExeFile;

        HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.pid);
        if (processHandle != nullptr) {
            wchar_t pathBuffer[MAX_PATH]{};
            DWORD pathSize = static_cast<DWORD>(std::size(pathBuffer));
            if (QueryFullProcessImageNameW(processHandle, 0, pathBuffer, &pathSize)) {
                info.imagePath = pathBuffer;
                if (info.name.empty()) {
                    info.name = FileNameFromPath(info.imagePath);
                }
            }
            CloseHandle(processHandle);
        }

        if (ContainsInsensitive(info.name, filter) || ContainsInsensitive(info.imagePath, filter)) {
            processes.push_back(std::move(info));
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);

    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.pid < b.pid;
    });

    return processes;
}

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminSid = nullptr;

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &adminSid)) {
        CheckTokenMembership(nullptr, adminSid, &isAdmin);
        FreeSid(adminSid);
    }

    return isAdmin == TRUE;
}

bool RelaunchAsAdmin() {
    wchar_t executablePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0) {
        return false;
    }

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.hwnd = nullptr;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = executablePath;
    executeInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&executeInfo)) {
        return false;
    }

    if (executeInfo.hProcess) {
        CloseHandle(executeInfo.hProcess);
    }

    return true;
}

} // namespace rf::core
