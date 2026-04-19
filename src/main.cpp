#include <Windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/AssetIO.h"
#include "core/AssetRipperBridge.h"
#include "core/Dx11PreviewRenderer.h"
#include "core/Injector.h"
#include "core/Logger.h"
#include "core/MemoryScanner.h"
#include "core/ProcessUtils.h"
#include "core/Settings.h"
#include "plugins/PluginManager.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using namespace rf;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"RipperForgeImGuiMainWindow";
constexpr wchar_t kWindowTitle[] = L"RipperForge - Asset Injection + Reverse Toolkit";
constexpr uint64_t kAutoCaptureScanIntervalMs = 1500;

constexpr std::array<const char*, 6> kScanTypeLabels = {
    "int32",
    "int64",
    "float",
    "double",
    "utf8_string",
    "byte_array",
};

constexpr std::array<const char*, 6> kScanCompareLabels = {
    "exact",
    "changed",
    "unchanged",
    "increased",
    "decreased",
    "equals",
};

constexpr std::array<const wchar_t*, 3> kHookEngines = {
    L"Unity",
    L"Source",
    L"Unreal",
};

constexpr std::array<const wchar_t*, 2> kHookBackends = {
    L"MinHook",
    L"Detours",
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring GetModuleDirectory() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    return std::filesystem::path(modulePath).parent_path().wstring();
}

template <size_t N>
void SetBuffer(std::array<char, N>& buffer, const std::string& value) {
    buffer.fill('\0');
    if (value.empty()) {
        return;
    }
    strncpy_s(buffer.data(), buffer.size(), value.c_str(), _TRUNCATE);
}

template <size_t N>
std::string BufferString(const std::array<char, N>& buffer) {
    return std::string(buffer.data());
}

template <size_t N>
void SetBuffer(std::array<char, N>& buffer, const std::wstring& value) {
    SetBuffer(buffer, WideToUtf8(value));
}

bool ParseAddress(const std::string& text, uintptr_t& outAddress) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const char c : text) {
        if (c != ' ' && c != '\t') {
            normalized.push_back(c);
        }
    }

    if (normalized.empty()) {
        return false;
    }
    if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
        normalized = normalized.substr(2);
    }
    if (normalized.empty()) {
        return false;
    }

    char* end = nullptr;
    outAddress = static_cast<uintptr_t>(std::strtoull(normalized.c_str(), &end, 16));
    return end != nullptr && *end == '\0';
}

std::string FormatAddress(uintptr_t address) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << address;
    return stream.str();
}

std::string HrToHex(HRESULT hr) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

std::string Win32ErrorToString(DWORD errorCode) {
    if (errorCode == 0) {
        return "success";
    }

    LPSTR messageBuffer = nullptr;
    const DWORD messageLength = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string message;
    if (messageLength > 0 && messageBuffer != nullptr) {
        message.assign(messageBuffer, messageLength);
        LocalFree(messageBuffer);
    } else {
        message = "Unknown Win32 error";
    }

    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }

    return message;
}

core::TypedScanValueType TypeFromIndex(int index) {
    if (index < 0 || index >= static_cast<int>(kScanTypeLabels.size())) {
        return core::TypedScanValueType::Int32;
    }
    return static_cast<core::TypedScanValueType>(index);
}

int IndexFromType(core::TypedScanValueType type) {
    return static_cast<int>(type);
}

core::TypedScanCompareMode CompareModeFromIndex(int index) {
    if (index < 0 || index >= static_cast<int>(kScanCompareLabels.size())) {
        return core::TypedScanCompareMode::Exact;
    }
    return static_cast<core::TypedScanCompareMode>(index);
}

int IndexFromCompareMode(core::TypedScanCompareMode mode) {
    return static_cast<int>(mode);
}

std::wstring TypeToWString(core::TypedScanValueType type) {
    return Utf8ToWide(kScanTypeLabels[IndexFromType(type)]);
}

core::TypedScanValueType TypeFromWString(const std::wstring& value) {
    const std::string narrowed = WideToUtf8(value);
    for (int i = 0; i < static_cast<int>(kScanTypeLabels.size()); ++i) {
        if (_stricmp(narrowed.c_str(), kScanTypeLabels[static_cast<size_t>(i)]) == 0) {
            return TypeFromIndex(i);
        }
    }
    return core::TypedScanValueType::Int32;
}

std::wstring CompareToWString(core::TypedScanCompareMode mode) {
    return Utf8ToWide(kScanCompareLabels[IndexFromCompareMode(mode)]);
}

core::TypedScanCompareMode CompareFromWString(const std::wstring& value) {
    const std::string narrowed = WideToUtf8(value);
    for (int i = 0; i < static_cast<int>(kScanCompareLabels.size()); ++i) {
        if (_stricmp(narrowed.c_str(), kScanCompareLabels[static_cast<size_t>(i)]) == 0) {
            return CompareModeFromIndex(i);
        }
    }
    return core::TypedScanCompareMode::Exact;
}

size_t DefaultTypeByteSize(core::TypedScanValueType type) {
    switch (type) {
    case core::TypedScanValueType::Int32:
    case core::TypedScanValueType::Float:
        return 4;
    case core::TypedScanValueType::Int64:
    case core::TypedScanValueType::Double:
        return 8;
    case core::TypedScanValueType::Utf8String:
        return 32;
    case core::TypedScanValueType::ByteArray:
        return 16;
    default:
        return 4;
    }
}

bool BrowseOpenFile(HWND owner, const wchar_t* filter, std::wstring& outPath) {
    wchar_t fileBuffer[MAX_PATH]{};
    OPENFILENAMEW openFile{};
    openFile.lStructSize = sizeof(openFile);
    openFile.hwndOwner = owner;
    openFile.lpstrFilter = filter;
    openFile.lpstrFile = fileBuffer;
    openFile.nMaxFile = MAX_PATH;
    openFile.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&openFile)) {
        return false;
    }

    outPath = fileBuffer;
    return true;
}

bool BrowseDirectory(HWND owner, std::wstring& outPath) {
    BROWSEINFOW browseInfo{};
    browseInfo.hwndOwner = owner;
    browseInfo.lpszTitle = L"Select directory";
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
    if (itemList == nullptr) {
        return false;
    }

    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(itemList, path) == TRUE;
    CoTaskMemFree(itemList);
    if (!ok) {
        return false;
    }

    outPath = path;
    return true;
}

bool BrowseSaveFile(
    HWND owner,
    const wchar_t* filter,
    const wchar_t* defaultExt,
    const std::wstring& initialName,
    std::wstring& outPath) {

    wchar_t fileBuffer[MAX_PATH]{};
    if (!initialName.empty()) {
        wcsncpy_s(fileBuffer, initialName.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW saveFile{};
    saveFile.lStructSize = sizeof(saveFile);
    saveFile.hwndOwner = owner;
    saveFile.lpstrFilter = filter;
    saveFile.lpstrFile = fileBuffer;
    saveFile.nMaxFile = MAX_PATH;
    saveFile.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    saveFile.lpstrDefExt = defaultExt;

    if (!GetSaveFileNameW(&saveFile)) {
        return false;
    }

    outPath = fileBuffer;
    return true;
}

template <size_t N>
std::vector<std::filesystem::path> EnumerateFilesWithExtensions(
    const std::wstring& root,
    const std::array<const wchar_t*, N>& extensions) {

    std::vector<std::filesystem::path> files;
    if (root.empty()) {
        return files;
    }

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return files;
    }

    auto isMatch = [&](const std::filesystem::path& path) {
        std::wstring ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(towlower(c));
        });
        for (const auto* candidate : extensions) {
            if (ext == candidate) {
                return true;
            }
        }
        return false;
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (isMatch(entry.path())) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

bool ParseOffsetsCsv(const std::string& csv, std::vector<uintptr_t>& offsets) {
    offsets.clear();
    std::stringstream stream(csv);
    std::string token;
    while (std::getline(stream, token, ',')) {
        std::string trimmed;
        for (const char c : token) {
            if (c != ' ' && c != '\t') {
                trimmed.push_back(c);
            }
        }
        if (trimmed.empty()) {
            continue;
        }
        uintptr_t value = 0;
        if (!ParseAddress(trimmed, value)) {
            return false;
        }
        offsets.push_back(value);
    }
    return true;
}

void ApplyIndustrialTheme(const std::string& densitySetting) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.30f, 0.33f, 0.38f, 0.85f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.21f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.33f, 0.36f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.49f, 0.73f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.49f, 0.73f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.62f, 0.80f, 0.98f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.27f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.34f, 0.43f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.16f, 0.19f, 0.23f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.27f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.32f, 0.41f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.31f, 0.34f, 0.39f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.31f, 0.40f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.45f, 0.71f, 0.97f, 0.60f);

    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    if (_stricmp(densitySetting.c_str(), "cozy") == 0) {
        style.WindowPadding = ImVec2(10.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.CellPadding = ImVec2(8.0f, 6.0f);
    } else {
        style.WindowPadding = ImVec2(7.0f, 6.0f);
        style.FramePadding = ImVec2(6.0f, 3.0f);
        style.ItemSpacing = ImVec2(6.0f, 4.0f);
        style.CellPadding = ImVec2(6.0f, 4.0f);
    }
}

struct TypedScanRow {
    uintptr_t address = 0;
    std::string value;
};

struct TypedScanJobResult {
    uint64_t jobId = 0;
    core::TypedScanSession session;
    std::string error;
    uint64_t elapsedMs = 0;
};

struct PatternScanJobResult {
    uint64_t jobId = 0;
    std::vector<uintptr_t> addresses;
    std::string error;
    uint64_t elapsedMs = 0;
};

struct CaptureScanJobResult {
    uint64_t jobId = 0;
    std::vector<std::filesystem::path> textures;
    std::vector<std::filesystem::path> models;
    std::string error;
    uint64_t elapsedMs = 0;
    bool logResult = false;
};

struct WatchEntry {
    uint64_t id = 0;
    uintptr_t address = 0;
    core::TypedScanValueType type = core::TypedScanValueType::Int32;
    size_t byteSize = 4;
    bool freeze = false;
    std::array<char, 64> label{};
    std::array<char, 128> freezeValue{};
    std::string currentValue;
    std::string status;
    std::chrono::steady_clock::time_point nextPoll{};
    std::chrono::steady_clock::time_point nextFreeze{};
};

class RipperForgeApp {
public:
    bool Initialize(HINSTANCE instance);
    int Run();
    ~RipperForgeApp();
    const std::string& InitFailureReason() const { return initFailureReason_; }

private:
    static LRESULT CALLBACK WindowProcSetup(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(HINSTANCE instance);
    bool CreateD3D();
    void CleanupD3D();
    bool CreateRenderTarget();
    void CleanupRenderTarget();

    void LoadState();
    void SaveState();
    void EnsureDefaultScanRange();

    void BeginFrame();
    void EndFrame();
    void RenderDockspaceAndPanels();
    void BuildDefaultDockLayout(ImGuiID dockspaceId);

    void RenderProcessPanel();
    void RenderWorkspacePanel();
    void RenderInjectorTab();
    void RenderAssetRipperTab();
    void RenderHookManagerTab();
    void RenderReverseToolkitTab();
    void RenderPluginsTab();
    void RenderLogPanel();

    void HandleHotkeys();

    void RefreshProcessList(bool logRefresh);
    DWORD SelectedPid() const;
    bool InjectDllFromBuffer(const std::array<char, 1024>& pathBuffer, const char* contextLabel);

    void StartCapture();
    void StopCapture();
    void RequestCaptureScan(bool logResult);
    void PollCaptureScanJob();

    void LoadTextureAsset(const std::filesystem::path& path);
    void LoadModelAsset(const std::filesystem::path& path);
    void ExportCurrentTexturePng();
    void ExportCurrentMeshObj();
    void ExportCurrentMeshFbx();

    void GenerateHookTemplate();

    void StartTypedScanFirst();
    void StartTypedScanNext();
    void CancelTypedScan();
    void PollTypedScanJob();

    void StartPatternScan();
    void CancelPatternScan();
    void PollPatternScanJob();

    void ResolvePointerChainFromUi();
    void AddSelectedScanResultToWatch();
    void UpdateWatchList();

    void PlacePreviewHost(const ImVec2& minScreen, const ImVec2& maxScreen);

private:
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND previewHost_ = nullptr;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> deviceContext_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> mainRenderTargetView_;

    bool initialized_ = false;
    bool done_ = false;
    bool dockLayoutBuilt_ = false;
    bool previewVisibleThisFrame_ = false;
    bool previewReady_ = false;
    bool showLogAutoScroll_ = true;

    std::wstring moduleDir_;
    std::wstring configPath_;
    std::wstring pluginDir_;
    std::string imguiIniPathUtf8_;

    core::AppSettings settings_;

    std::array<char, 256> processFilter_{};
    std::array<char, 1024> injectDllPath_{};
    std::array<char, 1024> captureDllPath_{};
    std::array<char, 1024> captureOutputDir_{};
    std::array<char, 1024> hookDllPath_{};
    std::array<char, 256> typedScanValue_{};
    std::array<char, 32> scanRangeStart_{};
    std::array<char, 32> scanRangeEnd_{};
    std::array<char, 256> patternScanInput_{};
    std::array<char, 32> pointerBaseAddress_{};
    std::array<char, 256> pointerOffsets_{};
    std::array<char, 64> pointerResolvedAddress_{};

    int selectedProcessIndex_ = -1;
    DWORD selectedPid_ = 0;

    bool autoRefreshProcesses_ = true;
    uint32_t processRefreshIntervalMs_ = 2000;
    std::chrono::steady_clock::time_point nextProcessRefresh_{};

    int selectedHookEngine_ = 0;
    int selectedHookBackend_ = 0;
    bool captureRunning_ = false;
    std::chrono::steady_clock::time_point nextAutoCaptureScan_{};

    int selectedTextureIndex_ = -1;
    int selectedModelIndex_ = -1;

    int typedScanTypeIndex_ = 0;
    int typedScanCompareIndex_ = 0;
    bool typedScanHasSession_ = false;
    int selectedTypedResultIndex_ = -1;
    core::TypedScanSession typedScanSession_{};
    std::vector<TypedScanRow> typedScanRows_;
    std::string typedScanStatusLine_;

    bool typedScanRunning_ = false;
    uint64_t typedScanJobCounter_ = 0;
    uint64_t activeTypedScanJob_ = 0;
    std::shared_ptr<std::atomic_bool> typedScanCancelToken_;
    std::future<TypedScanJobResult> typedScanFuture_;

    bool patternScanRunning_ = false;
    uint64_t patternScanJobCounter_ = 0;
    uint64_t activePatternScanJob_ = 0;
    std::shared_ptr<std::atomic_bool> patternScanCancelToken_;
    std::future<PatternScanJobResult> patternScanFuture_;
    std::vector<uintptr_t> patternScanResults_;

    bool captureScanRunning_ = false;
    bool queuedCaptureScan_ = false;
    bool queuedCaptureScanLog_ = false;
    uint64_t captureScanJobCounter_ = 0;
    uint64_t activeCaptureScanJob_ = 0;
    std::future<CaptureScanJobResult> captureScanFuture_;

    std::vector<core::ProcessInfo> processes_;
    std::vector<std::filesystem::path> capturedTextures_;
    std::vector<std::filesystem::path> capturedModels_;
    core::TextureData currentTexture_{};
    core::MeshData currentMesh_{};

    core::AssetRipperBridge assetBridge_;
    core::Dx11PreviewRenderer previewRenderer_;
    plugins::PluginManager pluginManager_;

    std::vector<WatchEntry> watchList_;
    uint64_t watchIdCounter_ = 1;
    uint32_t freezeIntervalMs_ = 120;

    std::chrono::steady_clock::time_point lastFrameTime_{};
    std::string initFailureReason_;
};

bool RipperForgeApp::CreateMainWindow(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = &RipperForgeApp::WindowProcSetup;
    wc.hInstance = instance_;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassExW(&wc)) {
        const DWORD errorCode = GetLastError();
        if (errorCode != ERROR_CLASS_ALREADY_EXISTS) {
            initFailureReason_ = "RegisterClassExW failed: " + Win32ErrorToString(errorCode);
            return false;
        }
    }

    hwnd_ = CreateWindowW(
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1680,
        980,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        initFailureReason_ = "CreateWindowW failed: " + Win32ErrorToString(GetLastError());
        return false;
    }

    return true;
}

bool RipperForgeApp::CreateD3D() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevelOut = D3D_FEATURE_LEVEL_11_0;

    HRESULT lastHr = E_FAIL;
    auto tryCreateDevice = [&](D3D_DRIVER_TYPE driverType, UINT flags) -> bool {
        swapChain_.Reset();
        device_.Reset();
        deviceContext_.Reset();

        const HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            driverType,
            nullptr,
            flags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &sd,
            swapChain_.GetAddressOf(),
            device_.GetAddressOf(),
            &featureLevelOut,
            deviceContext_.GetAddressOf());

        lastHr = hr;
        return SUCCEEDED(hr);
    };

    bool created = tryCreateDevice(D3D_DRIVER_TYPE_HARDWARE, createFlags);
    if (!created && (createFlags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        created = tryCreateDevice(D3D_DRIVER_TYPE_HARDWARE, createFlags & ~D3D11_CREATE_DEVICE_DEBUG);
    }
    if (!created) {
        created = tryCreateDevice(D3D_DRIVER_TYPE_WARP, createFlags & ~D3D11_CREATE_DEVICE_DEBUG);
    }
    if (!created) {
        initFailureReason_ = "D3D11CreateDeviceAndSwapChain failed: " + HrToHex(lastHr);
        return false;
    }

    if (!CreateRenderTarget()) {
        initFailureReason_ = "CreateRenderTarget failed after D3D init.";
        return false;
    }

    return true;
}

bool RipperForgeApp::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) {
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, mainRenderTargetView_.GetAddressOf()))) {
        return false;
    }
    return mainRenderTargetView_ != nullptr;
}

void RipperForgeApp::CleanupRenderTarget() {
    mainRenderTargetView_.Reset();
}

void RipperForgeApp::CleanupD3D() {
    CleanupRenderTarget();
    swapChain_.Reset();
    deviceContext_.Reset();
    device_.Reset();
}

bool RipperForgeApp::Initialize(HINSTANCE instance) {
    initFailureReason_.clear();

    if (!CreateMainWindow(instance)) {
        if (initFailureReason_.empty()) {
            initFailureReason_ = "CreateMainWindow failed.";
        }
        return false;
    }
    if (!CreateD3D()) {
        if (initFailureReason_.empty()) {
            initFailureReason_ = "CreateD3D failed.";
        }
        return false;
    }

    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

    moduleDir_ = GetModuleDirectory();
    configPath_ = (std::filesystem::path(moduleDir_) / L"config" / L"settings.json").wstring();
    pluginDir_ = (std::filesystem::path(moduleDir_) / L"plugins").wstring();
    imguiIniPathUtf8_ = WideToUtf8((std::filesystem::path(moduleDir_) / L"config" / L"imgui_layout.ini").wstring());
    io.IniFilename = imguiIniPathUtf8_.c_str();

    if (!ImGui_ImplWin32_Init(hwnd_)) {
        initFailureReason_ = "ImGui_ImplWin32_Init failed.";
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_.Get(), deviceContext_.Get())) {
        initFailureReason_ = "ImGui_ImplDX11_Init failed.";
        return false;
    }

    LoadState();

    ApplyIndustrialTheme(WideToUtf8(settings_.uiDensity));

    previewHost_ = CreateWindowExW(
        WS_EX_NOPARENTNOTIFY,
        L"STATIC",
        L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0,
        0,
        10,
        10,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    if (previewHost_ != nullptr) {
        std::string previewError;
        if (previewRenderer_.Initialize(previewHost_, previewError)) {
            previewReady_ = true;
            ShowWindow(previewHost_, SW_HIDE);
        } else {
            core::Logger::Instance().Error("DX11 preview init failed: " + previewError);
            previewReady_ = false;
        }
    }

    assetBridge_.Initialize(moduleDir_);
    assetBridge_.SetCaptureDllPath(Utf8ToWide(BufferString(captureDllPath_)));
    assetBridge_.SetOutputDirectory(Utf8ToWide(BufferString(captureOutputDir_)));

    pluginManager_.Reload(pluginDir_);
    RefreshProcessList(true);
    RequestCaptureScan(false);

    core::Logger::Instance().Info("ImGui docked UI initialized. Multi-viewport is disabled.");
    if (!core::IsRunningAsAdmin()) {
        core::Logger::Instance().Info("Run as administrator for reliable injection into protected processes.");
    }

    lastFrameTime_ = std::chrono::steady_clock::now();
    nextProcessRefresh_ = lastFrameTime_ + std::chrono::milliseconds(processRefreshIntervalMs_);
    nextAutoCaptureScan_ = lastFrameTime_ + std::chrono::milliseconds(kAutoCaptureScanIntervalMs);
    initialized_ = true;
    return true;
}

void RipperForgeApp::EnsureDefaultScanRange() {
    if (BufferString(scanRangeStart_).empty() || BufferString(scanRangeEnd_).empty()) {
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        SetBuffer(scanRangeStart_, FormatAddress(reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress)));
        SetBuffer(scanRangeEnd_, FormatAddress(reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress)));
    }
}

void RipperForgeApp::LoadState() {
    settings_ = core::LoadSettings(configPath_);

    autoRefreshProcesses_ = settings_.autoRefresh;
    processRefreshIntervalMs_ = std::max<uint32_t>(500, settings_.refreshIntervalMs);
    freezeIntervalMs_ = std::max<uint32_t>(30, settings_.reverseToolkitFreezeIntervalMs);

    SetBuffer(processFilter_, settings_.processFilter);
    SetBuffer(injectDllPath_, settings_.lastDllPath);
    SetBuffer(captureDllPath_, settings_.captureDllPath);
    SetBuffer(captureOutputDir_, settings_.captureOutputDir);
    SetBuffer(hookDllPath_, settings_.hookDllPath);

    typedScanTypeIndex_ = IndexFromType(TypeFromWString(settings_.reverseToolkitScanDefaults.valueType));
    typedScanCompareIndex_ = IndexFromCompareMode(CompareFromWString(settings_.reverseToolkitScanDefaults.compareMode));
    SetBuffer(typedScanValue_, settings_.reverseToolkitScanDefaults.valueInput);
    SetBuffer(scanRangeStart_, settings_.reverseToolkitScanDefaults.rangeStartHex);
    SetBuffer(scanRangeEnd_, settings_.reverseToolkitScanDefaults.rangeEndHex);
    EnsureDefaultScanRange();

    selectedHookBackend_ = (_wcsicmp(settings_.hookBackend.c_str(), L"Detours") == 0) ? 1 : 0;

    if (BufferString(captureDllPath_).empty()) {
        SetBuffer(captureDllPath_, L"ripper_new6.dll");
    }
    if (BufferString(captureOutputDir_).empty()) {
        SetBuffer(captureOutputDir_, (std::filesystem::path(moduleDir_) / L"captures").wstring());
    }

    watchList_.clear();
    for (const auto& persisted : settings_.reverseToolkitWatchList) {
        WatchEntry entry;
        entry.id = watchIdCounter_++;
        entry.address = static_cast<uintptr_t>(persisted.address);
        entry.type = TypeFromWString(persisted.valueType);
        entry.byteSize = DefaultTypeByteSize(entry.type);
        entry.freeze = persisted.freeze;
        SetBuffer(entry.label, persisted.label.empty() ? Utf8ToWide(FormatAddress(entry.address)) : persisted.label);
        SetBuffer(entry.freezeValue, persisted.freezeValue);
        watchList_.push_back(entry);
    }
}

void RipperForgeApp::SaveState() {
    settings_.autoRefresh = autoRefreshProcesses_;
    settings_.refreshIntervalMs = processRefreshIntervalMs_;
    settings_.processFilter = Utf8ToWide(BufferString(processFilter_));
    settings_.lastDllPath = Utf8ToWide(BufferString(injectDllPath_));
    settings_.captureDllPath = Utf8ToWide(BufferString(captureDllPath_));
    settings_.captureOutputDir = Utf8ToWide(BufferString(captureOutputDir_));
    settings_.hookDllPath = Utf8ToWide(BufferString(hookDllPath_));
    settings_.hookBackend = kHookBackends[static_cast<size_t>(selectedHookBackend_)];

    settings_.uiLayout = L"dockspace-default";
    if (settings_.uiTheme.empty()) {
        settings_.uiTheme = L"industrial-dark";
    }
    if (settings_.uiDensity.empty()) {
        settings_.uiDensity = L"compact";
    }

    settings_.reverseToolkitScanDefaults.valueType = TypeToWString(TypeFromIndex(typedScanTypeIndex_));
    settings_.reverseToolkitScanDefaults.compareMode = CompareToWString(CompareModeFromIndex(typedScanCompareIndex_));
    settings_.reverseToolkitScanDefaults.valueInput = Utf8ToWide(BufferString(typedScanValue_));
    settings_.reverseToolkitScanDefaults.rangeStartHex = Utf8ToWide(BufferString(scanRangeStart_));
    settings_.reverseToolkitScanDefaults.rangeEndHex = Utf8ToWide(BufferString(scanRangeEnd_));
    settings_.reverseToolkitFreezeIntervalMs = freezeIntervalMs_;

    settings_.reverseToolkitWatchList.clear();
    for (const auto& entry : watchList_) {
        core::ReverseToolkitWatchEntry persisted;
        persisted.label = Utf8ToWide(BufferString(entry.label));
        persisted.address = static_cast<uint64_t>(entry.address);
        persisted.valueType = TypeToWString(entry.type);
        persisted.freezeValue = Utf8ToWide(BufferString(entry.freezeValue));
        persisted.freeze = entry.freeze;
        settings_.reverseToolkitWatchList.push_back(std::move(persisted));
    }

    core::SaveSettings(configPath_, settings_);
}

RipperForgeApp::~RipperForgeApp() {
    if (!initialized_) {
        return;
    }

    done_ = true;

    CancelTypedScan();
    CancelPatternScan();

    if (typedScanFuture_.valid()) {
        typedScanFuture_.wait();
    }
    if (patternScanFuture_.valid()) {
        patternScanFuture_.wait();
    }
    if (captureScanFuture_.valid()) {
        captureScanFuture_.wait();
    }

    assetBridge_.StopCapture();
    pluginManager_.UnloadAll();
    SaveState();

    if (previewReady_) {
        previewRenderer_.Shutdown();
    }
    if (previewHost_ != nullptr) {
        DestroyWindow(previewHost_);
        previewHost_ = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupD3D();

    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kWindowClassName, instance_);
}

LRESULT CALLBACK RipperForgeApp::WindowProcSetup(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = reinterpret_cast<RipperForgeApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    auto* self = reinterpret_cast<RipperForgeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->WindowProc(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT RipperForgeApp::WindowProc(UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd_, message, wParam, lParam)) {
        return true;
    }

    switch (message) {
    case WM_SIZE:
        if (device_ != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            swapChain_->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            if (!CreateRenderTarget()) {
                core::Logger::Instance().Error("Failed to recreate D3D render target after resize.");
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

int RipperForgeApp::Run() {
    MSG message{};
    while (!done_) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                done_ = true;
            }
        }
        if (done_) {
            break;
        }

        BeginFrame();
        RenderDockspaceAndPanels();
        EndFrame();
    }

    return static_cast<int>(message.wParam);
}

void RipperForgeApp::BeginFrame() {
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds =
        std::chrono::duration_cast<std::chrono::duration<float>>(now - lastFrameTime_).count();
    lastFrameTime_ = now;

    PollTypedScanJob();
    PollPatternScanJob();
    PollCaptureScanJob();
    UpdateWatchList();

    if (autoRefreshProcesses_ && now >= nextProcessRefresh_) {
        RefreshProcessList(false);
        nextProcessRefresh_ = now + std::chrono::milliseconds(processRefreshIntervalMs_);
    }

    if (captureRunning_ && now >= nextAutoCaptureScan_) {
        RequestCaptureScan(false);
        nextAutoCaptureScan_ = now + std::chrono::milliseconds(kAutoCaptureScanIntervalMs);
    }

    previewVisibleThisFrame_ = false;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    HandleHotkeys();

    if (previewReady_ && previewHost_ != nullptr && IsWindowVisible(previewHost_)) {
        previewRenderer_.Render(std::max(0.0001f, deltaSeconds));
    }
}

void RipperForgeApp::EndFrame() {
    if (!previewVisibleThisFrame_ && previewHost_ != nullptr) {
        ShowWindow(previewHost_, SW_HIDE);
    }

    ImGui::Render();
    const float clearColor[4] = {0.07f, 0.08f, 0.10f, 1.0f};
    deviceContext_->OMSetRenderTargets(1, mainRenderTargetView_.GetAddressOf(), nullptr);
    deviceContext_->ClearRenderTargetView(mainRenderTargetView_.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

void RipperForgeApp::BuildDefaultDockLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID dockMain = dockspaceId;
    ImGuiID dockLeft = 0;
    ImGuiID dockBottom = 0;
    ImGuiID dockRight = 0;

    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.27f, &dockLeft, &dockRight);
    ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.28f, &dockBottom, &dockMain);

    ImGui::DockBuilderDockWindow("Process Control", dockLeft);
    ImGui::DockBuilderDockWindow("Workspace", dockMain);
    ImGui::DockBuilderDockWindow("Log Console", dockBottom);

    ImGui::DockBuilderFinish(dockspaceId);
}

void RipperForgeApp::RenderDockspaceAndPanels() {
    const ImGuiWindowFlags hostWindowFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("RipperForgeHost", nullptr, hostWindowFlags);
    ImGui::PopStyleVar(2);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Actions")) {
            if (ImGui::MenuItem("Refresh Processes", "F5")) {
                RefreshProcessList(true);
            }
            if (ImGui::MenuItem("Inject Selected DLL", "Ctrl+I")) {
                InjectDllFromBuffer(injectDllPath_, "Injector");
            }
            if (ImGui::MenuItem(captureRunning_ ? "Stop Capture" : "Start Capture", "Ctrl+R")) {
                if (captureRunning_) {
                    StopCapture();
                } else {
                    StartCapture();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::TextUnformatted(captureRunning_ ? "Capture: RUNNING" : "Capture: IDLE");
        ImGui::Separator();
        ImGui::Text("PID: %lu", static_cast<unsigned long>(SelectedPid()));
        ImGui::EndMenuBar();
    }

    const ImGuiID dockspaceId = ImGui::GetID("MainDockspace");
    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockFlags);
    if (!dockLayoutBuilt_) {
        BuildDefaultDockLayout(dockspaceId);
        dockLayoutBuilt_ = true;
    }
    ImGui::End();

    RenderProcessPanel();
    RenderWorkspacePanel();
    RenderLogPanel();
}

void RipperForgeApp::HandleHotkeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
        RefreshProcessList(true);
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I)) {
        InjectDllFromBuffer(injectDllPath_, "Injector");
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
        if (captureRunning_) {
            StopCapture();
        } else {
            StartCapture();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F6)) {
        if (typedScanRunning_) {
            CancelTypedScan();
        } else if (typedScanHasSession_) {
            StartTypedScanNext();
        } else {
            StartTypedScanFirst();
        }
    }
}

void RipperForgeApp::RenderProcessPanel() {
    if (!ImGui::Begin("Process Control")) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Process Browser");
    ImGui::Separator();

    ImGui::InputTextWithHint("##procfilter", "Filter by name/path", processFilter_.data(), processFilter_.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        RefreshProcessList(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Elevate")) {
        if (!core::IsRunningAsAdmin()) {
            if (core::RelaunchAsAdmin()) {
                core::Logger::Instance().Info("Relaunching as administrator.");
            } else {
                core::Logger::Instance().Error("Elevation prompt failed.");
            }
        } else {
            core::Logger::Instance().Info("Already running as administrator.");
        }
    }

    ImGui::Checkbox("Auto Refresh", &autoRefreshProcesses_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    int interval = static_cast<int>(processRefreshIntervalMs_);
    if (ImGui::SliderInt("Refresh ms", &interval, 500, 10000)) {
        processRefreshIntervalMs_ = static_cast<uint32_t>(interval);
    }

    if (ImGui::BeginTable("process_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.33f);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.67f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < processes_.size(); ++i) {
            const auto& process = processes_[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool isSelected = selectedPid_ == process.pid;
            ImGui::PushID(static_cast<int>(process.pid));
            if (ImGui::Selectable(WideToUtf8(process.name).c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedProcessIndex_ = static_cast<int>(i);
                selectedPid_ = process.pid;
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lu", static_cast<unsigned long>(process.pid));

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(WideToUtf8(process.imagePath).c_str());
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void RipperForgeApp::RenderWorkspacePanel() {
    if (!ImGui::Begin("Workspace")) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("workspace_tabs")) {
        if (ImGui::BeginTabItem("Injector")) {
            RenderInjectorTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Asset Ripper")) {
            RenderAssetRipperTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Hooks")) {
            RenderHookManagerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Reverse Toolkit")) {
            RenderReverseToolkitTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Plugins")) {
            RenderPluginsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void RipperForgeApp::RenderInjectorTab() {
    ImGui::TextUnformatted("DLL Injection");
    ImGui::Separator();

    ImGui::InputText("DLL Path", injectDllPath_.data(), injectDllPath_.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse DLL")) {
        std::wstring path;
        if (BrowseOpenFile(hwnd_, L"DLL Files (*.dll)\0*.dll\0All Files (*.*)\0*.*\0", path)) {
            SetBuffer(injectDllPath_, path);
        }
    }

    if (ImGui::Button("Inject Selected Process")) {
        InjectDllFromBuffer(injectDllPath_, "Injector");
    }
}

void RipperForgeApp::RenderAssetRipperTab() {
    ImGui::TextUnformatted("AssetRIpper Capture + Export");
    ImGui::Separator();

    ImGui::InputText("Capture DLL", captureDllPath_.data(), captureDllPath_.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse Capture DLL")) {
        std::wstring path;
        if (BrowseOpenFile(hwnd_, L"DLL Files (*.dll)\0*.dll\0All Files (*.*)\0*.*\0", path)) {
            SetBuffer(captureDllPath_, path);
            assetBridge_.SetCaptureDllPath(path);
        }
    }

    ImGui::InputText("Output Directory", captureOutputDir_.data(), captureOutputDir_.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse Output")) {
        std::wstring path;
        if (BrowseDirectory(hwnd_, path)) {
            SetBuffer(captureOutputDir_, path);
            assetBridge_.SetOutputDirectory(path);
        }
    }

    if (!captureRunning_) {
        if (ImGui::Button("Start Capture")) {
            StartCapture();
        }
    } else {
        if (ImGui::Button("Stop Capture")) {
            StopCapture();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(captureScanRunning_ ? "Scanning..." : "Scan Assets")) {
        RequestCaptureScan(true);
    }

    float progress = assetBridge_.QueryCaptureProgress();
    if (progress < 0.0f) {
        const size_t total = capturedTextures_.size() + capturedModels_.size();
        progress = std::min(0.95f, static_cast<float>(total) / 100.0f);
    }
    ImGui::ProgressBar(std::clamp(progress, 0.0f, 1.0f), ImVec2(-1, 0), captureRunning_ ? "Capture Running" : "Idle");

    if (ImGui::BeginTable("asset_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Lists", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("asset_lists")) {
            ImGui::Text("Textures (%d)", static_cast<int>(capturedTextures_.size()));
            if (ImGui::BeginListBox("##texture_list", ImVec2(-1, 170))) {
                for (int i = 0; i < static_cast<int>(capturedTextures_.size()); ++i) {
                    const bool selected = (selectedTextureIndex_ == i);
                    const std::string name = WideToUtf8(capturedTextures_[static_cast<size_t>(i)].filename().wstring());
                    ImGui::PushID(i);
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        selectedTextureIndex_ = i;
                    }
                    if (selected && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        LoadTextureAsset(capturedTextures_[static_cast<size_t>(i)]);
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            if (ImGui::Button("Load Texture") && selectedTextureIndex_ >= 0 &&
                selectedTextureIndex_ < static_cast<int>(capturedTextures_.size())) {
                LoadTextureAsset(capturedTextures_[static_cast<size_t>(selectedTextureIndex_)]);
            }

            ImGui::Separator();
            ImGui::Text("Models (%d)", static_cast<int>(capturedModels_.size()));
            if (ImGui::BeginListBox("##model_list", ImVec2(-1, 170))) {
                for (int i = 0; i < static_cast<int>(capturedModels_.size()); ++i) {
                    const bool selected = (selectedModelIndex_ == i);
                    const std::string name = WideToUtf8(capturedModels_[static_cast<size_t>(i)].filename().wstring());
                    ImGui::PushID(i);
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        selectedModelIndex_ = i;
                    }
                    if (selected && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        LoadModelAsset(capturedModels_[static_cast<size_t>(i)]);
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            if (ImGui::Button("Load Model") && selectedModelIndex_ >= 0 &&
                selectedModelIndex_ < static_cast<int>(capturedModels_.size())) {
                LoadModelAsset(capturedModels_[static_cast<size_t>(selectedModelIndex_)]);
            }
        }
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("preview_area", ImVec2(0, 0), true)) {
            ImGui::TextUnformatted("DirectX 11 Preview");
            ImGui::Separator();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ImVec2 slotSize(
                std::max(150.0f, available.x),
                std::max(200.0f, available.y - 52.0f));
            ImGui::InvisibleButton("##preview_slot", slotSize);
            const ImVec2 minScreen = ImGui::GetItemRectMin();
            const ImVec2 maxScreen = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(minScreen, maxScreen, IM_COL32(120, 145, 180, 180), 2.0f, 0, 1.5f);
            PlacePreviewHost(minScreen, maxScreen);

            if (ImGui::Button("Export PNG")) {
                ExportCurrentTexturePng();
            }
            ImGui::SameLine();
            if (ImGui::Button("Export OBJ")) {
                ExportCurrentMeshObj();
            }
            ImGui::SameLine();
            if (ImGui::Button("Export FBX")) {
                ExportCurrentMeshFbx();
            }
        }
        ImGui::EndChild();

        ImGui::EndTable();
    }
}

void RipperForgeApp::RenderHookManagerTab() {
    ImGui::TextUnformatted("Hook Manager");
    ImGui::Separator();

    ImGui::Combo("Engine Template", &selectedHookEngine_, "Unity\0Source\0Unreal\0");
    ImGui::Combo("Backend", &selectedHookBackend_, "MinHook\0Detours\0");

    ImGui::InputText("Hook DLL", hookDllPath_.data(), hookDllPath_.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse Hook DLL")) {
        std::wstring path;
        if (BrowseOpenFile(hwnd_, L"DLL Files (*.dll)\0*.dll\0All Files (*.*)\0*.*\0", path)) {
            SetBuffer(hookDllPath_, path);
        }
    }

    if (ImGui::Button("Generate Hook Template")) {
        GenerateHookTemplate();
    }
    ImGui::SameLine();
    if (ImGui::Button("Inject Hook DLL")) {
        InjectDllFromBuffer(hookDllPath_, "Hook");
    }

    if (selectedHookBackend_ == 0) {
        ImGui::TextUnformatted("MinHook selected: ensure MinHook headers/libs are available in your hook project.");
    } else {
        ImGui::TextUnformatted("Detours selected: ensure detours.h + detours.lib are configured.");
    }
}

void RipperForgeApp::RenderReverseToolkitTab() {
    ImGui::TextUnformatted("Reverse Toolkit");
    ImGui::Separator();

    if (ImGui::BeginTable("reverse_scan_controls", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Value Type");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##valuetype", &typedScanTypeIndex_, "int32\0int64\0float\0double\0utf8_string\0byte_array\0");

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Next Filter");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##nextfilter", &typedScanCompareIndex_, "exact\0changed\0unchanged\0increased\0decreased\0equals\0");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Scan Value");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##scanvalue", typedScanValue_.data(), typedScanValue_.size());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Range Start");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##rangestart", scanRangeStart_.data(), scanRangeStart_.size());

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Range End");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##rangeend", scanRangeEnd_.data(), scanRangeEnd_.size());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Actions");
        if (ImGui::Button(typedScanRunning_ ? "Scanning..." : "First Scan")) {
            StartTypedScanFirst();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next Scan")) {
            StartTypedScanNext();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel Scan")) {
            CancelTypedScan();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Result To Watch")) {
            AddSelectedScanResultToWatch();
        }

        ImGui::EndTable();
    }

    if (!typedScanStatusLine_.empty()) {
        ImGui::TextUnformatted(typedScanStatusLine_.c_str());
    }

    if (ImGui::BeginTable("typed_scan_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < typedScanRows_.size(); ++i) {
            const auto& row = typedScanRows_[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", static_cast<unsigned>(i));
            ImGui::TableSetColumnIndex(1);
            const bool selected = selectedTypedResultIndex_ == static_cast<int>(i);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(FormatAddress(row.address).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedTypedResultIndex_ = static_cast<int>(i);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.value.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Pointer Chain Explorer");
    if (ImGui::BeginTable("pointer_controls", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Base Address");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##baseaddress", pointerBaseAddress_.data(), pointerBaseAddress_.size());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Offsets (hex,csv)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##offsets", pointerOffsets_.data(), pointerOffsets_.size());

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button("Resolve Chain")) {
            ResolvePointerChainFromUi();
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Resolved Target");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##resolvedtarget", pointerResolvedAddress_.data(), pointerResolvedAddress_.size(), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Watch / Freeze");
    int freezeInterval = static_cast<int>(freezeIntervalMs_);
    if (ImGui::SliderInt("Freeze Interval (ms)", &freezeInterval, 30, 1000)) {
        freezeIntervalMs_ = static_cast<uint32_t>(freezeInterval);
    }

    std::vector<size_t> removeIndices;
    if (ImGui::BeginTable("watch_table", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Freeze Value", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Del", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < watchList_.size(); ++i) {
            auto& entry = watchList_[i];
            ImGui::PushID(static_cast<int>(entry.id));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##freeze", &entry.freeze);

            ImGui::TableSetColumnIndex(1);
            ImGui::InputText("##label", entry.label.data(), entry.label.size());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(FormatAddress(entry.address).c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(kScanTypeLabels[static_cast<size_t>(entry.type)]);

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(entry.currentValue.c_str());

            ImGui::TableSetColumnIndex(5);
            ImGui::InputText("##freeze_value", entry.freezeValue.data(), entry.freezeValue.size());

            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(entry.status.c_str());

            ImGui::TableSetColumnIndex(7);
            if (ImGui::Button("X")) {
                removeIndices.push_back(i);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    for (auto it = removeIndices.rbegin(); it != removeIndices.rend(); ++it) {
        watchList_.erase(watchList_.begin() + static_cast<ptrdiff_t>(*it));
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("AoB Pattern Scan (Legacy API)")) {
        ImGui::InputText("Pattern", patternScanInput_.data(), patternScanInput_.size());
        if (ImGui::Button(patternScanRunning_ ? "Pattern Scanning..." : "Start Pattern Scan")) {
            StartPatternScan();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel Pattern Scan")) {
            CancelPatternScan();
        }
        if (ImGui::BeginListBox("##pattern_results", ImVec2(-1, 130))) {
            for (size_t i = 0; i < patternScanResults_.size(); ++i) {
                const auto address = patternScanResults_[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Selectable(FormatAddress(address).c_str(), false);
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
    }
}

void RipperForgeApp::RenderPluginsTab() {
    if (ImGui::Button("Reload Plugins")) {
        pluginManager_.Reload(pluginDir_);
        core::Logger::Instance().Info("Plugin catalog refreshed.");
    }

    ImGui::Separator();
    const auto& plugins = pluginManager_.Plugins();
    for (const auto& plugin : plugins) {
        ImGui::BulletText("%s (%s)", plugin.name.c_str(), WideToUtf8(plugin.filePath).c_str());
    }
    if (plugins.empty()) {
        ImGui::TextUnformatted("No plugins loaded.");
    }
}

void RipperForgeApp::RenderLogPanel() {
    if (!ImGui::Begin("Log Console")) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Auto Scroll", &showLogAutoScroll_);
    ImGui::Separator();

    const auto lines = core::Logger::Instance().Snapshot();
    ImGui::BeginChild("logs_scroll");
    for (const auto& line : lines) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (showLogAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}

void RipperForgeApp::RefreshProcessList(bool logRefresh) {
    const std::wstring filter = Utf8ToWide(BufferString(processFilter_));
    const DWORD previousPid = selectedPid_;
    processes_ = core::EnumerateProcesses(filter);

    selectedPid_ = 0;
    selectedProcessIndex_ = -1;
    for (size_t i = 0; i < processes_.size(); ++i) {
        if (processes_[i].pid == previousPid) {
            selectedPid_ = previousPid;
            selectedProcessIndex_ = static_cast<int>(i);
            break;
        }
    }

    if (logRefresh) {
        core::Logger::Instance().Info("Process list refreshed: " + std::to_string(processes_.size()) + " entries.");
    }
}

DWORD RipperForgeApp::SelectedPid() const {
    return selectedPid_;
}

bool RipperForgeApp::InjectDllFromBuffer(const std::array<char, 1024>& pathBuffer, const char* contextLabel) {
    const DWORD pid = SelectedPid();
    if (pid == 0) {
        core::Logger::Instance().Error("Select a target process first.");
        return false;
    }

    const std::wstring dllPath = Utf8ToWide(BufferString(pathBuffer));
    if (dllPath.empty()) {
        core::Logger::Instance().Error("DLL path is empty.");
        return false;
    }

    std::string error;
    if (!core::InjectDll(pid, dllPath, error)) {
        core::Logger::Instance().Error(std::string(contextLabel) + " injection failed: " + error);
        return false;
    }

    core::Logger::Instance().Info(std::string(contextLabel) + " injection succeeded for PID " + std::to_string(pid) + ".");
    return true;
}

void RipperForgeApp::StartCapture() {
    const DWORD pid = SelectedPid();
    if (pid == 0) {
        core::Logger::Instance().Error("Select a process before capture.");
        return;
    }

    const std::wstring captureDll = Utf8ToWide(BufferString(captureDllPath_));
    const std::wstring outputDir = Utf8ToWide(BufferString(captureOutputDir_));
    if (captureDll.empty() || outputDir.empty()) {
        core::Logger::Instance().Error("Capture DLL and output directory are required.");
        return;
    }

    assetBridge_.SetCaptureDllPath(captureDll);
    assetBridge_.SetOutputDirectory(outputDir);

    std::string error;
    if (!assetBridge_.StartCapture(pid, error)) {
        core::Logger::Instance().Error("Capture start failed: " + error);
        return;
    }

    captureRunning_ = true;
    nextAutoCaptureScan_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAutoCaptureScanIntervalMs);
    core::Logger::Instance().Info("Asset capture started for PID " + std::to_string(pid) + ".");
    RequestCaptureScan(false);
}

void RipperForgeApp::StopCapture() {
    assetBridge_.StopCapture();
    captureRunning_ = false;
    core::Logger::Instance().Info("Capture stopped.");
    RequestCaptureScan(false);
}

void RipperForgeApp::RequestCaptureScan(bool logResult) {
    if (captureScanRunning_) {
        queuedCaptureScan_ = true;
        queuedCaptureScanLog_ = queuedCaptureScanLog_ || logResult;
        return;
    }

    const std::wstring outputDir = Utf8ToWide(BufferString(captureOutputDir_));
    if (outputDir.empty()) {
        core::Logger::Instance().Error("Capture output directory is empty.");
        return;
    }

    captureScanRunning_ = true;
    queuedCaptureScan_ = false;
    queuedCaptureScanLog_ = false;
    const uint64_t jobId = ++captureScanJobCounter_;
    activeCaptureScanJob_ = jobId;

    captureScanFuture_ = std::async(std::launch::async, [jobId, outputDir, logResult]() {
        CaptureScanJobResult result;
        result.jobId = jobId;
        result.logResult = logResult;

        const auto begin = std::chrono::steady_clock::now();
        try {
            constexpr std::array<const wchar_t*, 8> kTextureExtensions = {
                L".dds", L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff", L".gif",
            };
            constexpr std::array<const wchar_t*, 8> kModelExtensions = {
                L".obj", L".fbx", L".glb", L".gltf", L".ply", L".stl", L".dae", L".x",
            };

            result.textures = EnumerateFilesWithExtensions(outputDir, kTextureExtensions);
            result.models = EnumerateFilesWithExtensions(outputDir, kModelExtensions);
        } catch (const std::exception& ex) {
            result.error = ex.what();
        }

        result.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count());
        return result;
    });
}

void RipperForgeApp::PollCaptureScanJob() {
    if (!captureScanRunning_ || !captureScanFuture_.valid()) {
        return;
    }

    if (captureScanFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    CaptureScanJobResult result = captureScanFuture_.get();
    captureScanRunning_ = false;
    if (result.jobId != activeCaptureScanJob_) {
        return;
    }

    if (!result.error.empty()) {
        core::Logger::Instance().Error("Capture scan failed: " + result.error);
    } else {
        capturedTextures_ = std::move(result.textures);
        capturedModels_ = std::move(result.models);
        if (result.logResult) {
            core::Logger::Instance().Info(
                "Capture scan finished: " + std::to_string(capturedTextures_.size()) + " textures, " +
                std::to_string(capturedModels_.size()) + " models in " +
                std::to_string(result.elapsedMs) + " ms.");
        }
    }

    if (queuedCaptureScan_) {
        const bool logAgain = queuedCaptureScanLog_;
        queuedCaptureScan_ = false;
        queuedCaptureScanLog_ = false;
        RequestCaptureScan(logAgain);
    }
}

void RipperForgeApp::LoadTextureAsset(const std::filesystem::path& path) {
    std::string error;
    core::TextureData texture;
    if (!core::LoadTextureFromFile(path.wstring(), texture, error)) {
        core::Logger::Instance().Error("Texture load failed: " + error);
        return;
    }

    if (previewReady_) {
        if (!previewRenderer_.SetTexture(texture, error)) {
            core::Logger::Instance().Error("Preview texture upload failed: " + error);
            return;
        }
        previewRenderer_.SetMode(core::PreviewMode::Texture);
    }

    currentTexture_ = std::move(texture);
    settings_.lastTextureAssetPath = path.wstring();
    core::Logger::Instance().Info("Texture loaded: " + WideToUtf8(path.wstring()));
}

void RipperForgeApp::LoadModelAsset(const std::filesystem::path& path) {
    std::string error;
    core::MeshData mesh;
    if (!core::LoadMeshFromObj(path.wstring(), mesh, error)) {
        core::Logger::Instance().Error("Model load failed: " + error + " (preview currently expects OBJ).");
        return;
    }

    if (previewReady_) {
        if (!previewRenderer_.SetMesh(mesh, error)) {
            core::Logger::Instance().Error("Preview mesh upload failed: " + error);
            return;
        }
        previewRenderer_.SetMode(core::PreviewMode::Model);
    }

    currentMesh_ = std::move(mesh);
    settings_.lastModelAssetPath = path.wstring();
    core::Logger::Instance().Info("Model loaded: " + WideToUtf8(path.wstring()));
}

void RipperForgeApp::ExportCurrentTexturePng() {
    if (currentTexture_.rgba8.empty()) {
        core::Logger::Instance().Error("No texture loaded.");
        return;
    }

    std::wstring defaultName = L"capture_texture.png";
    if (!currentTexture_.sourcePath.empty()) {
        defaultName = std::filesystem::path(currentTexture_.sourcePath).stem().wstring() + L".png";
    }

    std::wstring outputPath;
    if (!BrowseSaveFile(
            hwnd_,
            L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0",
            L"png",
            defaultName,
            outputPath)) {
        return;
    }

    std::string error;
    if (!core::SaveTextureToPng(outputPath, currentTexture_, error)) {
        core::Logger::Instance().Error("PNG export failed: " + error);
        return;
    }

    core::Logger::Instance().Info("PNG exported: " + WideToUtf8(outputPath));
}

void RipperForgeApp::ExportCurrentMeshObj() {
    if (currentMesh_.vertices.empty() || currentMesh_.indices.empty()) {
        core::Logger::Instance().Error("No mesh loaded.");
        return;
    }

    std::wstring defaultName = L"capture_mesh.obj";
    if (!currentMesh_.sourcePath.empty()) {
        defaultName = std::filesystem::path(currentMesh_.sourcePath).stem().wstring() + L".obj";
    }

    std::wstring outputPath;
    if (!BrowseSaveFile(
            hwnd_,
            L"Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0",
            L"obj",
            defaultName,
            outputPath)) {
        return;
    }

    std::string error;
    if (!core::SaveMeshToObj(outputPath, currentMesh_, error)) {
        core::Logger::Instance().Error("OBJ export failed: " + error);
        return;
    }

    core::Logger::Instance().Info("OBJ exported: " + WideToUtf8(outputPath));
}

void RipperForgeApp::ExportCurrentMeshFbx() {
    if (currentMesh_.vertices.empty() || currentMesh_.indices.empty()) {
        core::Logger::Instance().Error("No mesh loaded.");
        return;
    }

    std::wstring defaultName = L"capture_mesh.fbx";
    if (!currentMesh_.sourcePath.empty()) {
        defaultName = std::filesystem::path(currentMesh_.sourcePath).stem().wstring() + L".fbx";
    }

    std::wstring outputPath;
    if (!BrowseSaveFile(
            hwnd_,
            L"Autodesk FBX (*.fbx)\0*.fbx\0All Files (*.*)\0*.*\0",
            L"fbx",
            defaultName,
            outputPath)) {
        return;
    }

    std::string error;
    if (!core::SaveMeshToFbxAscii(outputPath, currentMesh_, error)) {
        core::Logger::Instance().Error("FBX export failed: " + error);
        return;
    }

    core::Logger::Instance().Info("FBX exported: " + WideToUtf8(outputPath));
}

void RipperForgeApp::GenerateHookTemplate() {
    const std::wstring engine = kHookEngines[static_cast<size_t>(std::clamp(selectedHookEngine_, 0, 2))];
    const std::wstring backend = kHookBackends[static_cast<size_t>(std::clamp(selectedHookBackend_, 0, 1))];

    std::filesystem::path outputDir =
        std::filesystem::path(moduleDir_) / L"hooks" / (engine + L"_" + backend);
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        core::Logger::Instance().Error("Could not create hook output directory.");
        return;
    }

    std::filesystem::path outputPath = outputDir / (engine + L"HookTemplate.cpp");
    std::ofstream stream(outputPath, std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
        core::Logger::Instance().Error("Failed to write hook template file.");
        return;
    }

    stream << "// Generated by RipperForge\n";
    stream << "#include <Windows.h>\n";
    if (_wcsicmp(backend.c_str(), L"Detours") == 0) {
        stream << "#include <detours.h>\n\n";
        stream << "static decltype(&Sleep) g_originalSleep = &Sleep;\n";
        stream << "VOID WINAPI HookedSleep(DWORD ms) { g_originalSleep(ms); }\n\n";
        stream << "bool InstallHooks() {\n";
        stream << "    if (DetourTransactionBegin() != NO_ERROR) return false;\n";
        stream << "    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) return false;\n";
        stream << "    if (DetourAttach(&(PVOID&)g_originalSleep, HookedSleep) != NO_ERROR) return false;\n";
        stream << "    return DetourTransactionCommit() == NO_ERROR;\n";
        stream << "}\n";
        stream << "void RemoveHooks() {\n";
        stream << "    DetourTransactionBegin();\n";
        stream << "    DetourUpdateThread(GetCurrentThread());\n";
        stream << "    DetourDetach(&(PVOID&)g_originalSleep, HookedSleep);\n";
        stream << "    DetourTransactionCommit();\n";
        stream << "}\n";
    } else {
        stream << "#include <MinHook.h>\n\n";
        stream << "static decltype(&Sleep) g_originalSleep = &Sleep;\n";
        stream << "VOID WINAPI HookedSleep(DWORD ms) { g_originalSleep(ms); }\n\n";
        stream << "bool InstallHooks() {\n";
        stream << "    if (MH_Initialize() != MH_OK) return false;\n";
        stream << "    if (MH_CreateHook(&Sleep, &HookedSleep, reinterpret_cast<LPVOID*>(&g_originalSleep)) != MH_OK) return false;\n";
        stream << "    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;\n";
        stream << "}\n";
        stream << "void RemoveHooks() {\n";
        stream << "    MH_DisableHook(MH_ALL_HOOKS);\n";
        stream << "    MH_Uninitialize();\n";
        stream << "}\n";
    }
    stream << "BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {\n";
    stream << "    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(module); InstallHooks(); }\n";
    stream << "    else if (reason == DLL_PROCESS_DETACH) { RemoveHooks(); }\n";
    stream << "    return TRUE;\n";
    stream << "}\n";

    std::filesystem::path notesPath = outputDir / L"build_notes.txt";
    std::ofstream notes(notesPath, std::ios::binary | std::ios::trunc);
    if (notes.good()) {
        notes << "Backend: " << WideToUtf8(backend) << "\n";
        notes << "1) Configure include/lib paths for the backend.\n";
        notes << "2) Build x64 DLL.\n";
        notes << "3) Set built DLL path in Hook tab and inject.\n";
    }

    SetBuffer(hookDllPath_, (outputDir / (engine + L"Hook.dll")).wstring());
    settings_.hookBackend = backend;
    core::Logger::Instance().Info("Hook template generated: " + WideToUtf8(outputPath.wstring()));
}

void RipperForgeApp::StartTypedScanFirst() {
    if (typedScanRunning_) {
        core::Logger::Instance().Info("Typed scan already running.");
        return;
    }

    const DWORD pid = SelectedPid();
    if (pid == 0) {
        core::Logger::Instance().Error("Select a process before scanning.");
        return;
    }

    uintptr_t start = 0;
    uintptr_t end = 0;
    if (!ParseAddress(BufferString(scanRangeStart_), start) || !ParseAddress(BufferString(scanRangeEnd_), end) || end <= start) {
        core::Logger::Instance().Error("Invalid scan range.");
        return;
    }

    const core::TypedScanValueType type = TypeFromIndex(typedScanTypeIndex_);
    std::vector<uint8_t> queryValue;
    std::string parseError;
    if (!core::ParseTypedValueInput(type, BufferString(typedScanValue_), queryValue, parseError)) {
        core::Logger::Instance().Error("Scan value parse failed: " + parseError);
        return;
    }

    typedScanRows_.clear();
    selectedTypedResultIndex_ = -1;
    typedScanCancelToken_ = std::make_shared<std::atomic_bool>(false);
    typedScanRunning_ = true;
    typedScanStatusLine_ = "First scan in progress...";
    const uint64_t jobId = ++typedScanJobCounter_;
    activeTypedScanJob_ = jobId;

    typedScanFuture_ = std::async(std::launch::async, [jobId, pid, start, end, type, queryValue, cancelToken = typedScanCancelToken_]() {
        TypedScanJobResult result;
        result.jobId = jobId;
        const auto begin = std::chrono::steady_clock::now();
        core::FirstTypedScan(
            pid,
            start,
            end,
            type,
            queryValue,
            2500,
            result.session,
            result.error,
            cancelToken.get());
        result.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count());
        return result;
    });
}

void RipperForgeApp::StartTypedScanNext() {
    if (typedScanRunning_) {
        core::Logger::Instance().Info("Typed scan already running.");
        return;
    }
    if (!typedScanHasSession_ || typedScanSession_.addresses.empty()) {
        core::Logger::Instance().Error("No active typed scan session. Run First Scan first.");
        return;
    }

    std::vector<uint8_t> equalsValue;
    const auto compareMode = CompareModeFromIndex(typedScanCompareIndex_);
    if (compareMode == core::TypedScanCompareMode::Equals) {
        std::string parseError;
        if (!core::ParseTypedValueInput(
                TypeFromIndex(typedScanTypeIndex_),
                BufferString(typedScanValue_),
                equalsValue,
                parseError)) {
            core::Logger::Instance().Error("Equals value parse failed: " + parseError);
            return;
        }
    }

    typedScanCancelToken_ = std::make_shared<std::atomic_bool>(false);
    typedScanRunning_ = true;
    typedScanStatusLine_ = "Next scan in progress...";
    const uint64_t jobId = ++typedScanJobCounter_;
    activeTypedScanJob_ = jobId;
    const core::TypedScanSession previousSession = typedScanSession_;

    typedScanFuture_ = std::async(std::launch::async, [jobId, previousSession, compareMode, equalsValue, cancelToken = typedScanCancelToken_]() {
        TypedScanJobResult result;
        result.jobId = jobId;
        const auto begin = std::chrono::steady_clock::now();
        core::NextTypedScan(
            previousSession,
            compareMode,
            equalsValue,
            2500,
            result.session,
            result.error,
            cancelToken.get());
        result.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count());
        return result;
    });
}

void RipperForgeApp::CancelTypedScan() {
    if (typedScanCancelToken_) {
        typedScanCancelToken_->store(true);
    }
}

void RipperForgeApp::PollTypedScanJob() {
    if (!typedScanRunning_ || !typedScanFuture_.valid()) {
        return;
    }
    if (typedScanFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    TypedScanJobResult result = typedScanFuture_.get();
    typedScanRunning_ = false;
    if (result.jobId != activeTypedScanJob_) {
        return;
    }

    if (!result.error.empty()) {
        typedScanStatusLine_ = "Typed scan failed: " + result.error;
        core::Logger::Instance().Error(typedScanStatusLine_);
        return;
    }

    typedScanSession_ = std::move(result.session);
    typedScanHasSession_ = true;
    typedScanRows_.clear();
    for (size_t i = 0; i < typedScanSession_.addresses.size(); ++i) {
        TypedScanRow row;
        row.address = typedScanSession_.addresses[i];
        if (i < typedScanSession_.snapshots.size()) {
            std::string value;
            std::string conversionError;
            if (core::TypedValueBytesToString(typedScanSession_.type, typedScanSession_.snapshots[i], value, conversionError)) {
                row.value = value;
            } else {
                row.value = "<conversion failed>";
            }
        }
        typedScanRows_.push_back(std::move(row));
    }

    typedScanStatusLine_ =
        "Typed scan complete: " + std::to_string(typedScanRows_.size()) +
        " hits in " + std::to_string(result.elapsedMs) + " ms.";
    core::Logger::Instance().Info(typedScanStatusLine_);
}

void RipperForgeApp::StartPatternScan() {
    if (patternScanRunning_) {
        return;
    }

    const DWORD pid = SelectedPid();
    if (pid == 0) {
        core::Logger::Instance().Error("Select a process before AoB scan.");
        return;
    }

    std::vector<int> pattern;
    std::string error;
    if (!core::ParsePattern(BufferString(patternScanInput_), pattern, error)) {
        core::Logger::Instance().Error("Pattern parse failed: " + error);
        return;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const uintptr_t start = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    patternScanCancelToken_ = std::make_shared<std::atomic_bool>(false);
    patternScanRunning_ = true;
    const uint64_t jobId = ++patternScanJobCounter_;
    activePatternScanJob_ = jobId;

    patternScanFuture_ = std::async(std::launch::async, [jobId, pid, start, end, pattern, cancelToken = patternScanCancelToken_]() {
        PatternScanJobResult result;
        result.jobId = jobId;
        const auto begin = std::chrono::steady_clock::now();
        result.addresses = core::ScanPattern(pid, start, end, pattern, 512, result.error, cancelToken.get());
        result.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count());
        return result;
    });
}

void RipperForgeApp::CancelPatternScan() {
    if (patternScanCancelToken_) {
        patternScanCancelToken_->store(true);
    }
}

void RipperForgeApp::PollPatternScanJob() {
    if (!patternScanRunning_ || !patternScanFuture_.valid()) {
        return;
    }
    if (patternScanFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    PatternScanJobResult result = patternScanFuture_.get();
    patternScanRunning_ = false;
    if (result.jobId != activePatternScanJob_) {
        return;
    }

    if (!result.error.empty()) {
        core::Logger::Instance().Error("Pattern scan failed: " + result.error);
        return;
    }

    patternScanResults_ = std::move(result.addresses);
    core::Logger::Instance().Info(
        "Pattern scan complete: " + std::to_string(patternScanResults_.size()) +
        " hits in " + std::to_string(result.elapsedMs) + " ms.");
}

void RipperForgeApp::ResolvePointerChainFromUi() {
    const DWORD pid = SelectedPid();
    if (pid == 0) {
        core::Logger::Instance().Error("Select a process before pointer-chain resolve.");
        return;
    }

    uintptr_t baseAddress = 0;
    if (!ParseAddress(BufferString(pointerBaseAddress_), baseAddress)) {
        core::Logger::Instance().Error("Invalid pointer base address.");
        return;
    }

    std::vector<uintptr_t> offsets;
    if (!ParseOffsetsCsv(BufferString(pointerOffsets_), offsets)) {
        core::Logger::Instance().Error("Invalid offset list. Use hex CSV, e.g. 0x10,0x20.");
        return;
    }

    uintptr_t resolved = 0;
    std::string error;
    if (!core::ResolvePointerChain(pid, baseAddress, offsets, resolved, error)) {
        core::Logger::Instance().Error("Pointer chain resolve failed: " + error);
        return;
    }

    SetBuffer(pointerResolvedAddress_, FormatAddress(resolved));
    core::Logger::Instance().Info("Pointer chain resolved: " + FormatAddress(resolved));
}

void RipperForgeApp::AddSelectedScanResultToWatch() {
    if (selectedTypedResultIndex_ < 0 || selectedTypedResultIndex_ >= static_cast<int>(typedScanRows_.size())) {
        core::Logger::Instance().Error("Select a typed scan result first.");
        return;
    }

    const auto& row = typedScanRows_[static_cast<size_t>(selectedTypedResultIndex_)];
    WatchEntry entry;
    entry.id = watchIdCounter_++;
    entry.address = row.address;
    entry.type = TypeFromIndex(typedScanTypeIndex_);
    entry.byteSize = typedScanSession_.queryValue.empty() ? DefaultTypeByteSize(entry.type) : typedScanSession_.queryValue.size();
    entry.freeze = false;
    SetBuffer(entry.label, "watch_" + FormatAddress(row.address));
    SetBuffer(entry.freezeValue, row.value);
    entry.currentValue = row.value;
    watchList_.push_back(entry);
    core::Logger::Instance().Info("Watch entry created at " + FormatAddress(row.address));
}

void RipperForgeApp::UpdateWatchList() {
    const DWORD pid = SelectedPid();
    if (pid == 0 || watchList_.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto pollInterval = std::chrono::milliseconds(120);
    const auto freezeInterval = std::chrono::milliseconds(std::max<uint32_t>(30, freezeIntervalMs_));

    for (auto& entry : watchList_) {
        if (entry.nextPoll.time_since_epoch().count() == 0 || now >= entry.nextPoll) {
            std::vector<uint8_t> bytes;
            std::string error;
            if (core::ReadMemory(pid, entry.address, entry.byteSize, bytes, error)) {
                std::string value;
                if (core::TypedValueBytesToString(entry.type, bytes, value, error)) {
                    entry.currentValue = value;
                    entry.status = "ok";
                } else {
                    entry.status = "decode error";
                }
            } else {
                entry.status = "read failed";
            }
            entry.nextPoll = now + pollInterval;
        }

        if (entry.freeze && (entry.nextFreeze.time_since_epoch().count() == 0 || now >= entry.nextFreeze)) {
            std::vector<uint8_t> freezeBytes;
            std::string error;
            if (core::ParseTypedValueInput(entry.type, BufferString(entry.freezeValue), freezeBytes, error)) {
                if (entry.type == core::TypedScanValueType::Utf8String || entry.type == core::TypedScanValueType::ByteArray) {
                    entry.byteSize = freezeBytes.size();
                }
                if (!core::WriteMemory(pid, entry.address, freezeBytes, error)) {
                    entry.status = "freeze write failed";
                } else {
                    entry.status = "frozen";
                }
            } else {
                entry.status = "freeze parse failed";
            }
            entry.nextFreeze = now + freezeInterval;
        }
    }
}

void RipperForgeApp::PlacePreviewHost(const ImVec2& minScreen, const ImVec2& maxScreen) {
    if (!previewReady_ || previewHost_ == nullptr) {
        return;
    }

    POINT tl{static_cast<LONG>(minScreen.x), static_cast<LONG>(minScreen.y)};
    POINT br{static_cast<LONG>(maxScreen.x), static_cast<LONG>(maxScreen.y)};
    ScreenToClient(hwnd_, &tl);
    ScreenToClient(hwnd_, &br);

    const int width = std::max(1, static_cast<int>(br.x - tl.x));
    const int height = std::max(1, static_cast<int>(br.y - tl.y));
    SetWindowPos(previewHost_, HWND_TOP, tl.x, tl.y, width, height, SWP_NOACTIVATE);
    ShowWindow(previewHost_, SW_SHOWNA);
    previewRenderer_.Resize();
    previewVisibleThisFrame_ = true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    RipperForgeApp app;
    if (!app.Initialize(instance)) {
        std::string message = "Failed to initialize RipperForge ImGui runtime.";
        if (!app.InitFailureReason().empty()) {
            message += "\n\n";
            message += app.InitFailureReason();
        }
        MessageBoxA(nullptr, message.c_str(), "RipperForge", MB_ICONERROR | MB_OK);
        return 1;
    }
    return app.Run();
}
