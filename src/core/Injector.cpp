#include "core/Injector.h"

#include <Windows.h>

#include <filesystem>
#include <sstream>

namespace rf::core {

namespace {

std::string Win32ErrorToString(DWORD errorCode) {
    LPSTR messageBuffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string message;
    if (length > 0 && messageBuffer != nullptr) {
        message.assign(messageBuffer, length);
        LocalFree(messageBuffer);
    } else {
        message = "Unknown error";
    }

    return message;
}

} // namespace

bool InjectDll(DWORD pid, const std::wstring& dllPath, std::string& error) {
    if (pid == 0) {
        error = "No target process selected.";
        return false;
    }

    if (!std::filesystem::exists(dllPath)) {
        error = "DLL path does not exist.";
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        pid);
    if (process == nullptr) {
        error = "OpenProcess failed: " + Win32ErrorToString(GetLastError());
        return false;
    }

    const size_t dllBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteBuffer = VirtualAllocEx(process, nullptr, dllBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteBuffer == nullptr) {
        error = "VirtualAllocEx failed: " + Win32ErrorToString(GetLastError());
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remoteBuffer, dllPath.c_str(), dllBytes, nullptr)) {
        error = "WriteProcessMemory failed: " + Win32ErrorToString(GetLastError());
        VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibraryW = kernel32 != nullptr ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    if (loadLibraryW == nullptr) {
        error = "Could not resolve LoadLibraryW.";
        VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryW),
        remoteBuffer,
        0,
        nullptr);

    if (remoteThread == nullptr) {
        error = "CreateRemoteThread failed: " + Win32ErrorToString(GetLastError());
        VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(remoteThread, 10'000);

    DWORD remoteExitCode = 0;
    GetExitCodeThread(remoteThread, &remoteExitCode);

    CloseHandle(remoteThread);
    VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
    CloseHandle(process);

    if (remoteExitCode == 0) {
        error = "Remote LoadLibraryW returned null. Check architecture mismatch or anti-cheat protection.";
        return false;
    }

    return true;
}

} // namespace rf::core
