#pragma once

#include <Windows.h>

#include <string>

namespace rf::core {

bool InjectDll(DWORD pid, const std::wstring& dllPath, std::string& error);

} // namespace rf::core
