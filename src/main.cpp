
#include <Windows.h>
#include <Windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
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

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")

namespace {

using namespace rf;

constexpr wchar_t kWindowClassName[] = L"RipperForgeMainWindow";
constexpr wchar_t kWindowTitle[] = L"RipperForge - Native Modding Toolkit";

constexpr UINT_PTR kProcessRefreshTimerId = 10;
constexpr UINT_PTR kCaptureProgressTimerId = 11;
constexpr UINT_PTR kPreviewRenderTimerId = 12;

enum ControlId : int {
    IDC_SEARCH_EDIT = 1001,
    IDC_REFRESH_BUTTON,
    IDC_AUTO_REFRESH,
    IDC_ELEVATE_BUTTON,
    IDC_PROCESS_LIST,
    IDC_TAB_CONTROL,

    IDC_DLL_PATH_EDIT = 2001,
    IDC_DLL_BROWSE_BUTTON,
    IDC_DLL_INJECT_BUTTON,

    IDC_MEM_ADDRESS_EDIT = 3001,
    IDC_MEM_BYTES_EDIT,
    IDC_MEM_READ_BUTTON,
    IDC_MEM_WRITE_BUTTON,
    IDC_MEM_PATTERN_EDIT,
    IDC_MEM_SCAN_BUTTON,
    IDC_MEM_RESULT_LIST,

    IDC_CAPTURE_START_BUTTON = 4001,
    IDC_CAPTURE_STOP_BUTTON,
    IDC_CAPTURE_PROGRESS,
    IDC_CAPTURE_DLL_EDIT,
    IDC_CAPTURE_DLL_BROWSE,
    IDC_CAPTURE_OUTPUT_EDIT,
    IDC_CAPTURE_OUTPUT_BROWSE,
    IDC_CAPTURE_SCAN_BUTTON,
    IDC_CAPTURE_TEXTURE_LIST,
    IDC_CAPTURE_MODEL_LIST,
    IDC_CAPTURE_PREVIEW_HOST,
    IDC_EXPORT_PNG_BUTTON,
    IDC_EXPORT_OBJ_BUTTON,
    IDC_EXPORT_FBX_BUTTON,

    IDC_HOOK_ENGINE_COMBO = 5001,
    IDC_HOOK_DLL_EDIT,
    IDC_HOOK_DLL_BROWSE,
    IDC_HOOK_BACKEND_COMBO,
    IDC_HOOK_GEN_TEMPLATE,
    IDC_HOOK_INJECT,

    IDC_PLUGIN_RELOAD_BUTTON = 6001,
    IDC_PLUGIN_LIST,

    IDC_LOG_EDIT = 7001,
};

enum class TabIndex : int {
    Injector = 0,
    Memory,
    AssetRipper,
    Hooks,
    Plugins,
    Logs,
    Count,
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

std::wstring GetWindowTextString(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }

    std::wstring value(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::wstring GetModuleDirectory() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    return std::filesystem::path(modulePath).parent_path().wstring();
}

bool ParseHexAddress(const std::wstring& text, uintptr_t& outAddress) {
    std::wstring normalized = text;
    if (normalized.rfind(L"0x", 0) == 0 || normalized.rfind(L"0X", 0) == 0) {
        normalized = normalized.substr(2);
    }

    if (normalized.empty()) {
        return false;
    }

    wchar_t* end = nullptr;
    outAddress = static_cast<uintptr_t>(wcstoull(normalized.c_str(), &end, 16));
    return end != nullptr && *end == L'\0';
}

bool ParseHexByteList(const std::wstring& text, std::vector<uint8_t>& out) {
    out.clear();
    std::wstringstream stream(text);
    std::wstring token;

    while (stream >> token) {
        if (token.size() > 2) {
            return false;
        }

        wchar_t* end = nullptr;
        const auto value = wcstoul(token.c_str(), &end, 16);
        if (end == nullptr || *end != L'\0' || value > 0xFF) {
            return false;
        }

        out.push_back(static_cast<uint8_t>(value));
    }

    return true;
}

std::wstring BytesToHexString(const std::vector<uint8_t>& bytes) {
    std::wstringstream stream;
    stream << std::uppercase << std::hex << std::setfill(L'0');

    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) {
            stream << L' ';
        }
        stream << std::setw(2) << static_cast<int>(bytes[i]);
    }

    return stream.str();
}

class MainWindow {
public:
    bool Create(HINSTANCE instance) {
        instance_ = instance;

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &MainWindow::WindowProcSetup;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = darkBrush_;
        windowClass.lpszClassName = kWindowClassName;

        if (!RegisterClassExW(&windowClass)) {
            return false;
        }

        hwnd_ = CreateWindowExW(
            0,
            kWindowClassName,
            kWindowTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1400,
            860,
            nullptr,
            nullptr,
            instance_,
            this);

        if (hwnd_ == nullptr) {
            return false;
        }

        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProcSetup(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = reinterpret_cast<MainWindow*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return self->WindowProc(message, wParam, lParam);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_SIZE:
            OnSize();
            return 0;
        case WM_COMMAND:
            OnCommand(wParam);
            return 0;
        case WM_NOTIFY:
            return OnNotify(lParam);
        case WM_TIMER:
            OnTimer(wParam);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return OnControlColor(wParam, lParam);
        case WM_DESTROY:
            OnDestroy();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void OnCreate() {
        EnableDarkTitleBar();

        core::Logger::Instance().SetCallback([this](const std::string& line) {
            AppendLog(Utf8ToWide(line));
        });

        const std::wstring exeDir = GetModuleDirectory();
        configPath_ = (std::filesystem::path(exeDir) / L"config" / L"settings.json").wstring();
        pluginDir_ = (std::filesystem::path(exeDir) / L"plugins").wstring();

        settings_ = core::LoadSettings(configPath_);
        if (settings_.hookBackend.empty()) {
            settings_.hookBackend = L"MinHook";
        }

        CreateGlobalControls();
        CreateTabPages();
        LayoutControls();

        assetBridge_.Initialize(exeDir);
        if (settings_.captureDllPath.empty()) {
            settings_.captureDllPath = assetBridge_.CaptureDllPath();
        }
        if (settings_.captureOutputDir.empty()) {
            settings_.captureOutputDir = assetBridge_.OutputDirectory();
        }

        SetWindowTextW(searchEdit_, settings_.processFilter.c_str());
        SetWindowTextW(dllPathEdit_, settings_.lastDllPath.c_str());
        SetWindowTextW(captureDllEdit_, settings_.captureDllPath.c_str());
        SetWindowTextW(captureOutputEdit_, settings_.captureOutputDir.c_str());
        SetWindowTextW(hookDllEdit_, settings_.hookDllPath.c_str());
        Button_SetCheck(autoRefreshCheckbox_, settings_.autoRefresh ? BST_CHECKED : BST_UNCHECKED);
        SelectHookBackend(settings_.hookBackend);

        assetBridge_.SetCaptureDllPath(settings_.captureDllPath);
        assetBridge_.SetOutputDirectory(settings_.captureOutputDir);

        RefreshProcessList(true);
        RefreshCapturedAssets(false);

        std::string previewError;
        if (previewRenderer_.Initialize(assetPreviewHost_, previewError)) {
            previewInitialized_ = true;
            if (!settings_.lastTextureAssetPath.empty() && std::filesystem::exists(settings_.lastTextureAssetPath)) {
                LoadTextureAsset(settings_.lastTextureAssetPath);
            }
            if (!settings_.lastModelAssetPath.empty() && std::filesystem::exists(settings_.lastModelAssetPath)) {
                LoadModelAsset(settings_.lastModelAssetPath);
            }
        } else {
            core::Logger::Instance().Error("DX11 preview init failed: " + previewError);
        }

        SetTimer(hwnd_, kProcessRefreshTimerId, std::max<UINT>(500, settings_.refreshIntervalMs), nullptr);
        SetTimer(hwnd_, kCaptureProgressTimerId, 120, nullptr);
        SetTimer(hwnd_, kPreviewRenderTimerId, 16, nullptr);

        pluginManager_.Reload(pluginDir_);
        RefreshPluginList();

        core::Logger::Instance().Info("RipperForge initialized.");
        if (!core::IsRunningAsAdmin()) {
            core::Logger::Instance().Info("Run as administrator for reliable injection into protected processes.");
        }
    }

    void OnDestroy() {
        KillTimer(hwnd_, kProcessRefreshTimerId);
        KillTimer(hwnd_, kCaptureProgressTimerId);
        KillTimer(hwnd_, kPreviewRenderTimerId);

        settings_.processFilter = GetWindowTextString(searchEdit_);
        settings_.lastDllPath = GetWindowTextString(dllPathEdit_);
        settings_.captureDllPath = GetWindowTextString(captureDllEdit_);
        settings_.captureOutputDir = GetWindowTextString(captureOutputEdit_);
        settings_.hookDllPath = GetWindowTextString(hookDllEdit_);
        settings_.hookBackend = GetHookBackend();
        settings_.autoRefresh = Button_GetCheck(autoRefreshCheckbox_) == BST_CHECKED;

        assetBridge_.StopCapture();
        previewRenderer_.Shutdown();
        previewInitialized_ = false;

        core::SaveSettings(configPath_, settings_);
        pluginManager_.UnloadAll();
        core::Logger::Instance().SetCallback(nullptr);

        if (uiFont_ != nullptr) {
            DeleteObject(uiFont_);
            uiFont_ = nullptr;
        }

        if (darkBrush_ != nullptr) {
            DeleteObject(darkBrush_);
            darkBrush_ = nullptr;
        }

        if (inputBrush_ != nullptr) {
            DeleteObject(inputBrush_);
            inputBrush_ = nullptr;
        }
    }

    void EnableDarkTitleBar() {
        constexpr DWORD dwmUseImmersiveDarkMode = 20;
        const BOOL enabled = TRUE;
        DwmSetWindowAttribute(hwnd_, dwmUseImmersiveDarkMode, &enabled, sizeof(enabled));
    }

    void CreateGlobalControls() {
        uiFont_ = CreateFontW(
            -18,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH,
            L"Bahnschrift");

        searchEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            100,
            24,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_SEARCH_EDIT),
            instance_,
            nullptr);

        refreshButton_ = CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            90,
            24,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_REFRESH_BUTTON),
            instance_,
            nullptr);

        autoRefreshCheckbox_ = CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Auto Refresh",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0,
            0,
            120,
            24,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_AUTO_REFRESH),
            instance_,
            nullptr);

        elevateButton_ = CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Elevate",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            90,
            24,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_ELEVATE_BUTTON),
            instance_,
            nullptr);

        processList_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            0,
            0,
            100,
            100,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_PROCESS_LIST),
            instance_,
            nullptr);

        ListView_SetExtendedListViewStyle(processList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        AddProcessColumns();

        tabControl_ = CreateWindowExW(
            0,
            WC_TABCONTROLW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0,
            0,
            300,
            300,
            hwnd_,
            reinterpret_cast<HMENU>(IDC_TAB_CONTROL),
            instance_,
            nullptr);

        ApplyFontRecursively(hwnd_);
    }

    void AddProcessColumns() {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        column.pszText = const_cast<LPWSTR>(L"Process");
        column.cx = 180;
        column.iSubItem = 0;
        ListView_InsertColumn(processList_, 0, &column);

        column.pszText = const_cast<LPWSTR>(L"PID");
        column.cx = 85;
        column.iSubItem = 1;
        ListView_InsertColumn(processList_, 1, &column);

        column.pszText = const_cast<LPWSTR>(L"Path");
        column.cx = 600;
        column.iSubItem = 2;
        ListView_InsertColumn(processList_, 2, &column);
    }

    void CreateTabPages() {
        constexpr std::array<const wchar_t*, static_cast<size_t>(TabIndex::Count)> tabNames = {
            L"Injector",
            L"Memory",
            L"Asset Ripper",
            L"Hooks",
            L"Plugins",
            L"Logs",
        };

        for (size_t i = 0; i < tabNames.size(); ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(tabNames[i]);
            TabCtrl_InsertItem(tabControl_, static_cast<int>(i), &item);

            pageWindows_[i] = CreateWindowExW(
                0,
                WC_STATICW,
                L"",
                WS_CHILD | (i == 0 ? WS_VISIBLE : 0),
                0,
                0,
                100,
                100,
                tabControl_,
                nullptr,
                instance_,
                nullptr);
        }

        CreateInjectorTab(pageWindows_[static_cast<size_t>(TabIndex::Injector)]);
        CreateMemoryTab(pageWindows_[static_cast<size_t>(TabIndex::Memory)]);
        CreateAssetTab(pageWindows_[static_cast<size_t>(TabIndex::AssetRipper)]);
        CreateHookTab(pageWindows_[static_cast<size_t>(TabIndex::Hooks)]);
        CreatePluginTab(pageWindows_[static_cast<size_t>(TabIndex::Plugins)]);
        CreateLogTab(pageWindows_[static_cast<size_t>(TabIndex::Logs)]);

        ApplyFontRecursively(tabControl_);
    }

    void CreateInjectorTab(HWND parent) {
        CreateWindowExW(0, WC_STATICW, L"DLL to Inject", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, parent, nullptr, instance_, nullptr);

        dllPathEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            100,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_DLL_PATH_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Browse",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            90,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_DLL_BROWSE_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Inject Selected Process",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            190,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_DLL_INJECT_BUTTON),
            instance_,
            nullptr);
    }

    void CreateMemoryTab(HWND parent) {
        CreateWindowExW(0, WC_STATICW, L"Address (hex)", WS_CHILD | WS_VISIBLE, 0, 0, 120, 20, parent, nullptr, instance_, nullptr);

        memAddressEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            160,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_ADDRESS_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Read",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            80,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_READ_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(0, WC_STATICW, L"Bytes (hex)", WS_CHILD | WS_VISIBLE, 0, 0, 120, 20, parent, nullptr, instance_, nullptr);

        memBytesEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            360,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_BYTES_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Write",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            80,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_WRITE_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(0, WC_STATICW, L"Pattern (e.g. 48 8B ?? ??)", WS_CHILD | WS_VISIBLE, 0, 0, 190, 20, parent, nullptr, instance_, nullptr);

        memPatternEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            360,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_PATTERN_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Scan",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            80,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_SCAN_BUTTON),
            instance_,
            nullptr);

        memResultsList_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            0,
            0,
            300,
            200,
            parent,
            reinterpret_cast<HMENU>(IDC_MEM_RESULT_LIST),
            instance_,
            nullptr);
    }

    void CreateAssetTab(HWND parent) {
        CreateWindowExW(
            0,
            WC_STATICW,
            L"AssetRIpper capture bridge: inject ripper DLL + monitor output assets with live DX11 preview.",
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            760,
            20,
            parent,
            nullptr,
            instance_,
            nullptr);

        CreateWindowExW(0, WC_STATICW, L"Capture DLL", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, parent, nullptr, instance_, nullptr);
        captureDllEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            360,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_DLL_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Browse",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            90,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_DLL_BROWSE),
            instance_,
            nullptr);

        CreateWindowExW(0, WC_STATICW, L"Output Dir", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, parent, nullptr, instance_, nullptr);
        captureOutputEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            360,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_OUTPUT_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Browse",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            90,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_OUTPUT_BROWSE),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Start Batch Capture",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            170,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_START_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Stop",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            80,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_STOP_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Scan Assets",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            100,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_SCAN_BUTTON),
            instance_,
            nullptr);

        captureProgress_ = CreateWindowExW(
            0,
            PROGRESS_CLASSW,
            L"",
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            500,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_PROGRESS),
            instance_,
            nullptr);
        SendMessageW(captureProgress_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        CreateWindowExW(0, WC_STATICW, L"Textures", WS_CHILD | WS_VISIBLE, 0, 0, 80, 20, parent, nullptr, instance_, nullptr);
        assetTextureList_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            0,
            0,
            300,
            120,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_TEXTURE_LIST),
            instance_,
            nullptr);

        CreateWindowExW(0, WC_STATICW, L"Models", WS_CHILD | WS_VISIBLE, 0, 0, 80, 20, parent, nullptr, instance_, nullptr);
        assetModelList_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            0,
            0,
            300,
            120,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_MODEL_LIST),
            instance_,
            nullptr);

        assetPreviewHost_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_STATICW,
            L"",
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            320,
            260,
            parent,
            reinterpret_cast<HMENU>(IDC_CAPTURE_PREVIEW_HOST),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Export Preview (.png)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            170,
            28,
            parent,
            reinterpret_cast<HMENU>(IDC_EXPORT_PNG_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Export Mesh (.obj)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            170,
            28,
            parent,
            reinterpret_cast<HMENU>(IDC_EXPORT_OBJ_BUTTON),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Export Mesh (.fbx)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            170,
            28,
            parent,
            reinterpret_cast<HMENU>(IDC_EXPORT_FBX_BUTTON),
            instance_,
            nullptr);
    }

    void CreateHookTab(HWND parent) {
        CreateWindowExW(0, WC_STATICW, L"Engine", WS_CHILD | WS_VISIBLE, 0, 0, 120, 20, parent, nullptr, instance_, nullptr);

        hookEngineCombo_ = CreateWindowExW(
            0,
            WC_COMBOBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            0,
            0,
            220,
            200,
            parent,
            reinterpret_cast<HMENU>(IDC_HOOK_ENGINE_COMBO),
            instance_,
            nullptr);

        ComboBox_AddString(hookEngineCombo_, L"Unity");
        ComboBox_AddString(hookEngineCombo_, L"Source");
        ComboBox_AddString(hookEngineCombo_, L"Unreal");
        ComboBox_SetCurSel(hookEngineCombo_, 0);

        CreateWindowExW(0, WC_STATICW, L"Hook Backend", WS_CHILD | WS_VISIBLE, 0, 0, 120, 20, parent, nullptr, instance_, nullptr);
        hookBackendCombo_ = CreateWindowExW(
            0,
            WC_COMBOBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            0,
            0,
            220,
            120,
            parent,
            reinterpret_cast<HMENU>(IDC_HOOK_BACKEND_COMBO),
            instance_,
            nullptr);
        ComboBox_AddString(hookBackendCombo_, L"MinHook");
        ComboBox_AddString(hookBackendCombo_, L"Detours");
        ComboBox_SetCurSel(hookBackendCombo_, 0);

        CreateWindowExW(0, WC_STATICW, L"Hook DLL path", WS_CHILD | WS_VISIBLE, 0, 0, 120, 20, parent, nullptr, instance_, nullptr);

        hookDllEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            450,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_HOOK_DLL_EDIT),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Browse",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            90,
            24,
            parent,
            reinterpret_cast<HMENU>(IDC_HOOK_DLL_BROWSE),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Generate Template",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            160,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_HOOK_GEN_TEMPLATE),
            instance_,
            nullptr);

        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Inject Hook DLL",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            140,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_HOOK_INJECT),
            instance_,
            nullptr);
    }

    void CreatePluginTab(HWND parent) {
        CreateWindowExW(
            0,
            WC_BUTTONW,
            L"Reload Plugins",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            140,
            30,
            parent,
            reinterpret_cast<HMENU>(IDC_PLUGIN_RELOAD_BUTTON),
            instance_,
            nullptr);

        pluginList_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            0,
            0,
            400,
            300,
            parent,
            reinterpret_cast<HMENU>(IDC_PLUGIN_LIST),
            instance_,
            nullptr);
    }

    void CreateLogTab(HWND parent) {
        logEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_EDITW,
            L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0,
            0,
            400,
            300,
            parent,
            reinterpret_cast<HMENU>(IDC_LOG_EDIT),
            instance_,
            nullptr);

        const auto snapshot = core::Logger::Instance().Snapshot();
        for (const auto& line : snapshot) {
            AppendLog(Utf8ToWide(line));
        }
    }

    void LayoutControls() {
        RECT rect{};
        GetClientRect(hwnd_, &rect);

        const int clientWidth = rect.right - rect.left;
        const int clientHeight = rect.bottom - rect.top;

        const int margin = 12;
        const int topRowHeight = 30;
        const int leftPanelWidth = std::clamp(clientWidth / 3, 320, 520);

        const int searchWidth = std::max(150, leftPanelWidth - 260);
        MoveWindow(searchEdit_, margin, margin, searchWidth, 24, TRUE);
        MoveWindow(refreshButton_, margin + searchWidth + 8, margin, 80, 24, TRUE);
        MoveWindow(autoRefreshCheckbox_, margin + searchWidth + 96, margin, 110, 24, TRUE);
        MoveWindow(elevateButton_, margin + leftPanelWidth - 92, margin, 90, 24, TRUE);

        MoveWindow(
            processList_,
            margin,
            margin + topRowHeight + 4,
            leftPanelWidth,
            clientHeight - (margin * 2 + topRowHeight + 4),
            TRUE);

        const int rightX = margin + leftPanelWidth + margin;
        const int rightWidth = std::max(300, clientWidth - rightX - margin);
        const int rightHeight = std::max(200, clientHeight - margin * 2);

        MoveWindow(tabControl_, rightX, margin, rightWidth, rightHeight, TRUE);

        RECT tabRect{};
        GetClientRect(tabControl_, &tabRect);
        TabCtrl_AdjustRect(tabControl_, FALSE, &tabRect);

        for (HWND page : pageWindows_) {
            MoveWindow(page, tabRect.left, tabRect.top, tabRect.right - tabRect.left, tabRect.bottom - tabRect.top, TRUE);
        }

        LayoutInjectorPage();
        LayoutMemoryPage();
        LayoutAssetPage();
        LayoutHookPage();
        LayoutPluginPage();
        LayoutLogPage();
    }

    void LayoutInjectorPage() {
        HWND page = pageWindows_[static_cast<size_t>(TabIndex::Injector)];
        RECT rect{};
        GetClientRect(page, &rect);

        const int margin = 12;
        const int width = rect.right - rect.left;

        const HWND label = GetWindow(page, GW_CHILD);
        MoveWindow(label, margin, margin, 120, 20, TRUE);

        MoveWindow(dllPathEdit_, margin, margin + 24, width - 240, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_DLL_BROWSE_BUTTON), width - 220, margin + 24, 90, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_DLL_INJECT_BUTTON), margin, margin + 64, 220, 30, TRUE);
    }

    void LayoutMemoryPage() {
        HWND page = pageWindows_[static_cast<size_t>(TabIndex::Memory)];
        RECT rect{};
        GetClientRect(page, &rect);

        const int margin = 12;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        HWND child = GetWindow(page, GW_CHILD);
        MoveWindow(child, margin, margin, 120, 20, TRUE);

        MoveWindow(memAddressEdit_, margin, margin + 22, 200, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_MEM_READ_BUTTON), margin + 210, margin + 22, 80, 24, TRUE);

        child = GetWindow(child, GW_HWNDNEXT);
        child = GetWindow(child, GW_HWNDNEXT);
        child = GetWindow(child, GW_HWNDNEXT);
        MoveWindow(child, margin, margin + 54, 120, 20, TRUE);

        MoveWindow(memBytesEdit_, margin, margin + 76, width - 160, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_MEM_WRITE_BUTTON), width - 140, margin + 76, 80, 24, TRUE);

        child = GetWindow(child, GW_HWNDNEXT);
        child = GetWindow(child, GW_HWNDNEXT);
        child = GetWindow(child, GW_HWNDNEXT);
        MoveWindow(child, margin, margin + 110, 210, 20, TRUE);

        MoveWindow(memPatternEdit_, margin, margin + 132, width - 160, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_MEM_SCAN_BUTTON), width - 140, margin + 132, 80, 24, TRUE);

        MoveWindow(memResultsList_, margin, margin + 166, width - margin * 2, height - (margin + 178), TRUE);
    }

    void LayoutAssetPage() {
        HWND page = pageWindows_[static_cast<size_t>(TabIndex::AssetRipper)];
        RECT rect{};
        GetClientRect(page, &rect);

        const int margin = 12;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        const int leftWidth = std::clamp(width / 2, 360, 460);
        const int rightX = margin + leftWidth + margin;
        const int rightWidth = std::max(200, width - rightX - margin);

        HWND child = GetWindow(page, GW_CHILD);
        MoveWindow(child, margin, margin, width - margin * 2, 20, TRUE); // description

        child = GetWindow(child, GW_HWNDNEXT); // capture dll label
        MoveWindow(child, margin, margin + 28, 100, 20, TRUE);
        MoveWindow(captureDllEdit_, margin, margin + 48, leftWidth - 100, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_CAPTURE_DLL_BROWSE), margin + leftWidth - 92, margin + 48, 90, 24, TRUE);

        child = GetWindow(child, GW_HWNDNEXT); // capture dll edit
        child = GetWindow(child, GW_HWNDNEXT); // capture dll browse
        child = GetWindow(child, GW_HWNDNEXT); // output dir label
        MoveWindow(child, margin, margin + 76, 100, 20, TRUE);
        MoveWindow(captureOutputEdit_, margin, margin + 96, leftWidth - 100, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_CAPTURE_OUTPUT_BROWSE), margin + leftWidth - 92, margin + 96, 90, 24, TRUE);

        MoveWindow(GetDlgItem(page, IDC_CAPTURE_START_BUTTON), margin, margin + 128, 150, 30, TRUE);
        MoveWindow(GetDlgItem(page, IDC_CAPTURE_STOP_BUTTON), margin + 158, margin + 128, 80, 30, TRUE);
        MoveWindow(GetDlgItem(page, IDC_CAPTURE_SCAN_BUTTON), margin + 244, margin + 128, 100, 30, TRUE);

        MoveWindow(captureProgress_, margin, margin + 166, leftWidth, 22, TRUE);

        child = GetWindow(child, GW_HWNDNEXT); // output edit
        child = GetWindow(child, GW_HWNDNEXT); // output browse
        child = GetWindow(child, GW_HWNDNEXT); // start
        child = GetWindow(child, GW_HWNDNEXT); // stop
        child = GetWindow(child, GW_HWNDNEXT); // scan
        child = GetWindow(child, GW_HWNDNEXT); // progress
        child = GetWindow(child, GW_HWNDNEXT); // textures label
        MoveWindow(child, margin, margin + 194, 80, 20, TRUE);

        const int listsTop = margin + 214;
        const int listHeight = std::max(86, (height - listsTop - 120) / 2);
        MoveWindow(assetTextureList_, margin, listsTop, leftWidth, listHeight, TRUE);

        child = GetWindow(child, GW_HWNDNEXT); // texture list
        child = GetWindow(child, GW_HWNDNEXT); // models label
        MoveWindow(child, margin, listsTop + listHeight + 8, 80, 20, TRUE);
        MoveWindow(assetModelList_, margin, listsTop + listHeight + 28, leftWidth, listHeight, TRUE);

        const int previewTop = margin + 48;
        const int previewHeight = std::max(160, height - previewTop - 100);
        MoveWindow(assetPreviewHost_, rightX, previewTop, rightWidth, previewHeight, TRUE);

        MoveWindow(GetDlgItem(page, IDC_EXPORT_PNG_BUTTON), rightX, previewTop + previewHeight + 12, 170, 28, TRUE);
        MoveWindow(GetDlgItem(page, IDC_EXPORT_OBJ_BUTTON), rightX + 178, previewTop + previewHeight + 12, 170, 28, TRUE);
        MoveWindow(GetDlgItem(page, IDC_EXPORT_FBX_BUTTON), rightX, previewTop + previewHeight + 44, 170, 28, TRUE);
    }

    void LayoutHookPage() {
        HWND page = pageWindows_[static_cast<size_t>(TabIndex::Hooks)];
        RECT rect{};
        GetClientRect(page, &rect);

        const int margin = 12;
        const int width = rect.right - rect.left;

        HWND child = GetWindow(page, GW_CHILD);
        MoveWindow(child, margin, margin, 120, 20, TRUE);
        MoveWindow(hookEngineCombo_, margin, margin + 22, 220, 24, TRUE);

        child = GetWindow(child, GW_HWNDNEXT); // engine combo
        child = GetWindow(child, GW_HWNDNEXT); // backend label
        MoveWindow(child, margin, margin + 56, 120, 20, TRUE);
        MoveWindow(hookBackendCombo_, margin, margin + 78, 220, 24, TRUE);

        child = GetWindow(child, GW_HWNDNEXT); // backend combo
        child = GetWindow(child, GW_HWNDNEXT); // hook dll label
        MoveWindow(child, margin, margin + 110, 120, 20, TRUE);

        MoveWindow(hookDllEdit_, margin, margin + 132, width - margin * 2 - 98, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_HOOK_DLL_BROWSE), width - margin - 90, margin + 132, 90, 24, TRUE);
        MoveWindow(GetDlgItem(page, IDC_HOOK_GEN_TEMPLATE), margin, margin + 166, 180, 30, TRUE);
        MoveWindow(GetDlgItem(page, IDC_HOOK_INJECT), margin + 190, margin + 166, 150, 30, TRUE);
    }

    void LayoutPluginPage() {
        HWND page = pageWindows_[static_cast<size_t>(TabIndex::Plugins)];
        RECT rect{};
        GetClientRect(page, &rect);

        const int margin = 12;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        MoveWindow(GetDlgItem(page, IDC_PLUGIN_RELOAD_BUTTON), margin, margin, 140, 30, TRUE);
        MoveWindow(pluginList_, margin, margin + 40, width - margin * 2, height - margin * 2 - 40, TRUE);
    }

    void LayoutLogPage() {
        HWND page = pageWindows_[static_cast<size_t>(TabIndex::Logs)];
        RECT rect{};
        GetClientRect(page, &rect);

        const int margin = 12;
        MoveWindow(logEdit_, margin, margin, rect.right - margin * 2, rect.bottom - margin * 2, TRUE);
    }

    void OnSize() {
        if (tabControl_ != nullptr) {
            LayoutControls();
            if (previewInitialized_) {
                previewRenderer_.Resize();
            }
        }
    }

    void OnCommand(WPARAM wParam) {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == IDC_CAPTURE_TEXTURE_LIST && code == LBN_SELCHANGE) {
            LoadSelectedTextureFromList();
            return;
        }
        if (id == IDC_CAPTURE_MODEL_LIST && code == LBN_SELCHANGE) {
            LoadSelectedModelFromList();
            return;
        }
        if (id == IDC_HOOK_BACKEND_COMBO && code == CBN_SELCHANGE) {
            settings_.hookBackend = GetHookBackend();
            return;
        }

        switch (id) {
        case IDC_REFRESH_BUTTON:
            RefreshProcessList(true);
            break;
        case IDC_ELEVATE_BUTTON:
            HandleElevationRequest();
            break;
        case IDC_AUTO_REFRESH:
            settings_.autoRefresh = Button_GetCheck(autoRefreshCheckbox_) == BST_CHECKED;
            break;
        case IDC_DLL_BROWSE_BUTTON:
            BrowseForDll(dllPathEdit_);
            break;
        case IDC_DLL_INJECT_BUTTON:
            InjectFromControl(dllPathEdit_);
            break;
        case IDC_MEM_READ_BUTTON:
            HandleMemoryRead();
            break;
        case IDC_MEM_WRITE_BUTTON:
            HandleMemoryWrite();
            break;
        case IDC_MEM_SCAN_BUTTON:
            HandlePatternScan();
            break;
        case IDC_CAPTURE_START_BUTTON:
            StartCapture();
            break;
        case IDC_CAPTURE_STOP_BUTTON:
            StopCapture();
            break;
        case IDC_CAPTURE_DLL_BROWSE:
            BrowseForDll(captureDllEdit_);
            break;
        case IDC_CAPTURE_OUTPUT_BROWSE:
            BrowseForDirectory(captureOutputEdit_);
            break;
        case IDC_CAPTURE_SCAN_BUTTON:
            RefreshCapturedAssets(true);
            break;
        case IDC_EXPORT_PNG_BUTTON:
            ExportCurrentTexturePng();
            break;
        case IDC_EXPORT_OBJ_BUTTON:
            ExportCurrentMeshObj();
            break;
        case IDC_EXPORT_FBX_BUTTON:
            ExportCurrentMeshFbx();
            break;
        case IDC_HOOK_GEN_TEMPLATE:
            GenerateHookTemplate();
            break;
        case IDC_HOOK_DLL_BROWSE:
            BrowseForDll(hookDllEdit_);
            break;
        case IDC_HOOK_INJECT:
            InjectFromControl(hookDllEdit_);
            break;
        case IDC_PLUGIN_RELOAD_BUTTON:
            pluginManager_.Reload(pluginDir_);
            RefreshPluginList();
            break;
        default:
            break;
        }
    }

    LRESULT OnNotify(LPARAM lParam) {
        const auto* notify = reinterpret_cast<LPNMHDR>(lParam);
        if (notify->hwndFrom == tabControl_ && notify->code == TCN_SELCHANGE) {
            const int selected = TabCtrl_GetCurSel(tabControl_);
            for (size_t i = 0; i < pageWindows_.size(); ++i) {
                ShowWindow(pageWindows_[i], i == static_cast<size_t>(selected) ? SW_SHOW : SW_HIDE);
            }
            if (previewInitialized_ && selected == static_cast<int>(TabIndex::AssetRipper)) {
                previewRenderer_.Resize();
            }
            return 0;
        }

        return 0;
    }

    void OnTimer(WPARAM timerId) {
        if (timerId == kProcessRefreshTimerId) {
            if (Button_GetCheck(autoRefreshCheckbox_) == BST_CHECKED) {
                RefreshProcessList();
            }
            return;
        }

        if (timerId == kCaptureProgressTimerId) {
            if (assetBridge_.IsCaptureRunning()) {
                const core::CapturePollResult poll = assetBridge_.Poll();
                const int progress = static_cast<int>(std::clamp(poll.progress01, 0.0f, 1.0f) * 100.0f);
                SendMessageW(captureProgress_, PBM_SETPOS, progress, 0);

                if (poll.totalCount != lastCaptureAssetCount_) {
                    lastCaptureAssetCount_ = poll.totalCount;
                    RefreshCapturedAssets(false);
                }
            }
            return;
        }

        if (timerId == kPreviewRenderTimerId && previewInitialized_) {
            previewRenderer_.Render(1.0f / 60.0f);
        }
    }

    LRESULT OnControlColor(WPARAM wParam, LPARAM lParam) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);

        SetBkColor(dc, RGB(20, 20, 24));
        SetTextColor(dc, RGB(220, 220, 224));

        const int id = GetDlgCtrlID(control);
        if (id == IDC_SEARCH_EDIT || id == IDC_DLL_PATH_EDIT || id == IDC_MEM_ADDRESS_EDIT ||
            id == IDC_MEM_BYTES_EDIT || id == IDC_MEM_PATTERN_EDIT || id == IDC_HOOK_DLL_EDIT ||
            id == IDC_CAPTURE_DLL_EDIT || id == IDC_CAPTURE_OUTPUT_EDIT ||
            id == IDC_LOG_EDIT || id == IDC_MEM_RESULT_LIST || id == IDC_PLUGIN_LIST ||
            id == IDC_CAPTURE_TEXTURE_LIST || id == IDC_CAPTURE_MODEL_LIST) {
            return reinterpret_cast<LRESULT>(inputBrush_);
        }

        return reinterpret_cast<LRESULT>(darkBrush_);
    }

    void HandleElevationRequest() {
        if (core::IsRunningAsAdmin()) {
            core::Logger::Instance().Info("Already running with elevated privileges.");
            return;
        }

        if (!core::RelaunchAsAdmin()) {
            core::Logger::Instance().Error("Elevation prompt failed.");
            return;
        }

        core::Logger::Instance().Info("Relaunching as administrator.");
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    void RefreshProcessList(bool logRefresh = false) {
        const std::wstring filter = GetWindowTextString(searchEdit_);
        settings_.processFilter = filter;
        processes_ = core::EnumerateProcesses(filter);

        ListView_DeleteAllItems(processList_);

        int row = 0;
        for (const auto& process : processes_) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(process.name.c_str());
            item.lParam = static_cast<LPARAM>(process.pid);

            const int inserted = ListView_InsertItem(processList_, &item);
            if (inserted < 0) {
                continue;
            }

            const std::wstring pidText = std::to_wstring(process.pid);
            ListView_SetItemText(processList_, inserted, 1, const_cast<LPWSTR>(pidText.c_str()));
            ListView_SetItemText(
                processList_,
                inserted,
                2,
                const_cast<LPWSTR>(process.imagePath.empty() ? L"" : process.imagePath.c_str()));

            ++row;
        }

        if (logRefresh) {
            core::Logger::Instance().Info("Process list refreshed: " + std::to_string(processes_.size()) + " entries.");
        }
    }

    DWORD GetSelectedPid() const {
        const int selected = ListView_GetNextItem(processList_, -1, LVNI_SELECTED);
        if (selected < 0) {
            return 0;
        }

        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = selected;
        if (!ListView_GetItem(processList_, &item)) {
            return 0;
        }

        return static_cast<DWORD>(item.lParam);
    }

    void BrowseForDll(HWND targetEdit) {
        wchar_t fileBuffer[MAX_PATH]{};

        OPENFILENAMEW openFile{};
        openFile.lStructSize = sizeof(openFile);
        openFile.hwndOwner = hwnd_;
        openFile.lpstrFilter = L"DLL Files (*.dll)\0*.dll\0All Files (*.*)\0*.*\0";
        openFile.lpstrFile = fileBuffer;
        openFile.nMaxFile = MAX_PATH;
        openFile.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (!GetOpenFileNameW(&openFile)) {
            return;
        }

        SetWindowTextW(targetEdit, fileBuffer);
        if (targetEdit == dllPathEdit_) {
            settings_.lastDllPath = fileBuffer;
        } else if (targetEdit == captureDllEdit_) {
            settings_.captureDllPath = fileBuffer;
            assetBridge_.SetCaptureDllPath(settings_.captureDllPath);
        } else if (targetEdit == hookDllEdit_) {
            settings_.hookDllPath = fileBuffer;
        }
    }

    void BrowseForDirectory(HWND targetEdit) {
        BROWSEINFOW browseInfo{};
        browseInfo.hwndOwner = hwnd_;
        browseInfo.lpszTitle = L"Select directory";
        browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
        if (itemList == nullptr) {
            return;
        }

        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(itemList, path)) {
            SetWindowTextW(targetEdit, path);
            if (targetEdit == captureOutputEdit_) {
                settings_.captureOutputDir = path;
                assetBridge_.SetOutputDirectory(settings_.captureOutputDir);
            }
        }

        CoTaskMemFree(itemList);
    }

    bool PromptSavePath(const wchar_t* filter, const wchar_t* defaultExtension, const std::wstring& initialName, std::wstring& outPath) {
        wchar_t fileBuffer[MAX_PATH]{};
        if (!initialName.empty()) {
            wcsncpy_s(fileBuffer, initialName.c_str(), _TRUNCATE);
        }

        OPENFILENAMEW saveFile{};
        saveFile.lStructSize = sizeof(saveFile);
        saveFile.hwndOwner = hwnd_;
        saveFile.lpstrFilter = filter;
        saveFile.lpstrFile = fileBuffer;
        saveFile.nMaxFile = MAX_PATH;
        saveFile.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        saveFile.lpstrDefExt = defaultExtension;

        if (!GetSaveFileNameW(&saveFile)) {
            return false;
        }

        outPath = fileBuffer;
        return true;
    }

    void InjectFromControl(HWND editControl) {
        const DWORD pid = GetSelectedPid();
        if (pid == 0) {
            core::Logger::Instance().Error("Select a target process first.");
            return;
        }

        const std::wstring dllPath = GetWindowTextString(editControl);
        if (dllPath.empty()) {
            core::Logger::Instance().Error("DLL path is empty.");
            return;
        }

        std::string error;
        if (!core::InjectDll(pid, dllPath, error)) {
            core::Logger::Instance().Error("Injection failed: " + error);
            return;
        }

        if (editControl == dllPathEdit_) {
            settings_.lastDllPath = dllPath;
        } else if (editControl == hookDllEdit_) {
            settings_.hookDllPath = dllPath;
            core::Logger::Instance().Info("Injected hook DLL using backend template: " + WideToUtf8(GetHookBackend()));
        }

        core::Logger::Instance().Info("Injection succeeded for PID " + std::to_string(pid) + ".");
    }

    std::string AddressToHex(uintptr_t value) const {
        std::ostringstream stream;
        stream << std::hex << std::uppercase << value;
        return stream.str();
    }

    void HandleMemoryRead() {
        const DWORD pid = GetSelectedPid();
        if (pid == 0) {
            core::Logger::Instance().Error("Select a process before memory operations.");
            return;
        }

        uintptr_t address = 0;
        if (!ParseHexAddress(GetWindowTextString(memAddressEdit_), address)) {
            core::Logger::Instance().Error("Invalid address format.");
            return;
        }

        std::vector<uint8_t> requestedBytes;
        ParseHexByteList(GetWindowTextString(memBytesEdit_), requestedBytes);
        const size_t readSize = requestedBytes.empty() ? 16 : requestedBytes.size();

        std::vector<uint8_t> data;
        std::string error;
        if (!core::ReadMemory(pid, address, readSize, data, error)) {
            core::Logger::Instance().Error("Read failed: " + error);
            return;
        }

        SetWindowTextW(memBytesEdit_, BytesToHexString(data).c_str());
        core::Logger::Instance().Info("Read " + std::to_string(data.size()) + " bytes from 0x" + AddressToHex(address));
    }

    void HandleMemoryWrite() {
        const DWORD pid = GetSelectedPid();
        if (pid == 0) {
            core::Logger::Instance().Error("Select a process before memory operations.");
            return;
        }

        uintptr_t address = 0;
        if (!ParseHexAddress(GetWindowTextString(memAddressEdit_), address)) {
            core::Logger::Instance().Error("Invalid address format.");
            return;
        }

        std::vector<uint8_t> bytes;
        if (!ParseHexByteList(GetWindowTextString(memBytesEdit_), bytes) || bytes.empty()) {
            core::Logger::Instance().Error("Invalid byte list. Example: 90 90 90");
            return;
        }

        std::string error;
        if (!core::WriteMemory(pid, address, bytes, error)) {
            core::Logger::Instance().Error("Write failed: " + error);
            return;
        }

        core::Logger::Instance().Info("Wrote " + std::to_string(bytes.size()) + " bytes to 0x" + AddressToHex(address));
    }

    void HandlePatternScan() {
        const DWORD pid = GetSelectedPid();
        if (pid == 0) {
            core::Logger::Instance().Error("Select a process before scanning.");
            return;
        }

        std::vector<int> pattern;
        std::string error;
        if (!core::ParsePattern(WideToUtf8(GetWindowTextString(memPatternEdit_)), pattern, error)) {
            core::Logger::Instance().Error("Pattern parse failed: " + error);
            return;
        }

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);

        const uintptr_t start = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const uintptr_t end = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);

        core::Logger::Instance().Info("Scanning memory (this can take a while)...");
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        const auto results = core::ScanPattern(pid, start, end, pattern, 256, error);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));

        if (!error.empty()) {
            core::Logger::Instance().Error("Scan failed: " + error);
            return;
        }

        SendMessageW(memResultsList_, LB_RESETCONTENT, 0, 0);
        for (const auto address : results) {
            const std::wstring line = L"0x" + Utf8ToWide(AddressToHex(address));
            SendMessageW(memResultsList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }

        core::Logger::Instance().Info("Pattern scan complete: " + std::to_string(results.size()) + " hits.");
    }

    void StartCapture() {
        const DWORD pid = GetSelectedPid();
        if (pid == 0) {
            core::Logger::Instance().Error("Select a process before starting capture.");
            return;
        }

        settings_.captureDllPath = GetWindowTextString(captureDllEdit_);
        settings_.captureOutputDir = GetWindowTextString(captureOutputEdit_);
        if (settings_.captureDllPath.empty()) {
            core::Logger::Instance().Error("Capture DLL path is empty.");
            return;
        }
        if (settings_.captureOutputDir.empty()) {
            core::Logger::Instance().Error("Capture output directory is empty.");
            return;
        }

        assetBridge_.SetCaptureDllPath(settings_.captureDllPath);
        assetBridge_.SetOutputDirectory(settings_.captureOutputDir);

        std::string error;
        if (!assetBridge_.StartCapture(pid, error)) {
            core::Logger::Instance().Error("Capture start failed: " + error);
            return;
        }

        captureRunning_ = true;
        lastCaptureAssetCount_ = 0;
        SendMessageW(captureProgress_, PBM_SETPOS, 0, 0);
        core::Logger::Instance().Info("AssetRIpper capture started for PID " + std::to_string(pid) + ".");
    }

    void StopCapture() {
        assetBridge_.StopCapture();
        captureRunning_ = false;
        SendMessageW(captureProgress_, PBM_SETPOS, 0, 0);
        core::Logger::Instance().Info("Capture stopped.");
    }

    void RefreshCapturedAssets(bool logResult) {
        capturedTextureAssets_ = assetBridge_.EnumerateTextureAssets();
        capturedModelAssets_ = assetBridge_.EnumerateModelAssets();
        lastCaptureAssetCount_ = static_cast<uint32_t>(capturedTextureAssets_.size() + capturedModelAssets_.size());

        SendMessageW(assetTextureList_, LB_RESETCONTENT, 0, 0);
        for (const auto& path : capturedTextureAssets_) {
            const std::wstring display = path.filename().wstring();
            SendMessageW(assetTextureList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        }
        if (capturedTextureAssets_.empty()) {
            SendMessageW(assetTextureList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"<no texture assets found>"));
        }

        SendMessageW(assetModelList_, LB_RESETCONTENT, 0, 0);
        for (const auto& path : capturedModelAssets_) {
            const std::wstring display = path.filename().wstring();
            SendMessageW(assetModelList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        }
        if (capturedModelAssets_.empty()) {
            SendMessageW(assetModelList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"<no model assets found>"));
        }

        if (logResult) {
            core::Logger::Instance().Info(
                "Capture scan: " + std::to_string(capturedTextureAssets_.size()) + " textures, " +
                std::to_string(capturedModelAssets_.size()) + " models.");
        }
    }

    void LoadTextureAsset(const std::wstring& path) {
        std::string error;
        core::TextureData texture;
        if (!core::LoadTextureFromFile(path, texture, error)) {
            core::Logger::Instance().Error("Texture load failed: " + error);
            return;
        }

        if (previewInitialized_) {
            if (!previewRenderer_.SetTexture(texture, error)) {
                core::Logger::Instance().Error("DX11 texture upload failed: " + error);
                return;
            }
            previewRenderer_.SetMode(core::PreviewMode::Texture);
        }

        currentTexture_ = std::move(texture);
        settings_.lastTextureAssetPath = path;
        core::Logger::Instance().Info("Texture loaded: " + WideToUtf8(path));
    }

    void LoadModelAsset(const std::wstring& path) {
        std::string error;
        core::MeshData mesh;
        if (!core::LoadMeshFromObj(path, mesh, error)) {
            core::Logger::Instance().Error("Model load failed: " + error + " (preview currently supports OBJ input)");
            return;
        }

        if (previewInitialized_) {
            if (!previewRenderer_.SetMesh(mesh, error)) {
                core::Logger::Instance().Error("DX11 mesh upload failed: " + error);
                return;
            }
            previewRenderer_.SetMode(core::PreviewMode::Model);
        }

        currentMesh_ = std::move(mesh);
        settings_.lastModelAssetPath = path;
        core::Logger::Instance().Info("Model loaded: " + WideToUtf8(path));
    }

    void LoadSelectedTextureFromList() {
        const int selected = static_cast<int>(SendMessageW(assetTextureList_, LB_GETCURSEL, 0, 0));
        if (selected < 0 || static_cast<size_t>(selected) >= capturedTextureAssets_.size()) {
            return;
        }
        LoadTextureAsset(capturedTextureAssets_[static_cast<size_t>(selected)].wstring());
    }

    void LoadSelectedModelFromList() {
        const int selected = static_cast<int>(SendMessageW(assetModelList_, LB_GETCURSEL, 0, 0));
        if (selected < 0 || static_cast<size_t>(selected) >= capturedModelAssets_.size()) {
            return;
        }
        LoadModelAsset(capturedModelAssets_[static_cast<size_t>(selected)].wstring());
    }

    void ExportCurrentTexturePng() {
        if (currentTexture_.rgba8.empty()) {
            core::Logger::Instance().Error("No texture is loaded.");
            return;
        }

        std::wstring defaultName = L"capture_texture.png";
        if (!currentTexture_.sourcePath.empty()) {
            defaultName = std::filesystem::path(currentTexture_.sourcePath).stem().wstring() + L".png";
        }

        std::wstring outputPath;
        if (!PromptSavePath(
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

    void ExportCurrentMeshObj() {
        if (currentMesh_.vertices.empty() || currentMesh_.indices.empty()) {
            core::Logger::Instance().Error("No mesh is loaded.");
            return;
        }

        std::wstring defaultName = L"capture_mesh.obj";
        if (!currentMesh_.sourcePath.empty()) {
            defaultName = std::filesystem::path(currentMesh_.sourcePath).stem().wstring() + L".obj";
        }

        std::wstring outputPath;
        if (!PromptSavePath(
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

    void ExportCurrentMeshFbx() {
        if (currentMesh_.vertices.empty() || currentMesh_.indices.empty()) {
            core::Logger::Instance().Error("No mesh is loaded.");
            return;
        }

        std::wstring defaultName = L"capture_mesh.fbx";
        if (!currentMesh_.sourcePath.empty()) {
            defaultName = std::filesystem::path(currentMesh_.sourcePath).stem().wstring() + L".fbx";
        }

        std::wstring outputPath;
        if (!PromptSavePath(
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

    std::wstring GetHookBackend() const {
        const int selection = ComboBox_GetCurSel(hookBackendCombo_);
        if (selection < 0) {
            return L"MinHook";
        }

        wchar_t backend[64]{};
        ComboBox_GetLBTextW(hookBackendCombo_, selection, backend);
        return backend;
    }

    void SelectHookBackend(const std::wstring& backend) {
        const int count = ComboBox_GetCount(hookBackendCombo_);
        for (int i = 0; i < count; ++i) {
            wchar_t item[64]{};
            ComboBox_GetLBTextW(hookBackendCombo_, i, item);
            if (_wcsicmp(item, backend.c_str()) == 0) {
                ComboBox_SetCurSel(hookBackendCombo_, i);
                return;
            }
        }
        ComboBox_SetCurSel(hookBackendCombo_, 0);
    }

    void GenerateHookTemplate() {
        const int engineSelection = ComboBox_GetCurSel(hookEngineCombo_);
        if (engineSelection < 0) {
            core::Logger::Instance().Error("Select an engine first.");
            return;
        }

        wchar_t engine[64]{};
        ComboBox_GetLBTextW(hookEngineCombo_, engineSelection, engine);
        const std::wstring backend = GetHookBackend();

        std::filesystem::path outputDir = std::filesystem::path(GetModuleDirectory()) / L"hooks" /
            (std::wstring(engine) + L"_" + backend);
        std::error_code errorCode;
        std::filesystem::create_directories(outputDir, errorCode);
        if (errorCode) {
            core::Logger::Instance().Error("Could not create hooks directory.");
            return;
        }

        std::filesystem::path outputPath = outputDir / (std::wstring(engine) + L"HookTemplate.cpp");
        std::ofstream stream(outputPath, std::ios::binary | std::ios::trunc);
        if (!stream.good()) {
            core::Logger::Instance().Error("Failed to create hook template file.");
            return;
        }

        stream << "// Generated by RipperForge\n";
        stream << "// Engine: " << WideToUtf8(engine) << "\n\n";
        stream << "#include <Windows.h>\n";

        if (_wcsicmp(backend.c_str(), L"Detours") == 0) {
            stream << "#include <detours.h>\n\n";
            stream << "static decltype(&Sleep) g_originalSleep = &Sleep;\n\n";
            stream << "VOID WINAPI HookedSleep(DWORD ms) {\n";
            stream << "    g_originalSleep(ms);\n";
            stream << "}\n\n";
            stream << "bool InstallHooks() {\n";
            stream << "    if (DetourTransactionBegin() != NO_ERROR) return false;\n";
            stream << "    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) return false;\n";
            stream << "    if (DetourAttach(&(PVOID&)g_originalSleep, HookedSleep) != NO_ERROR) return false;\n";
            stream << "    return DetourTransactionCommit() == NO_ERROR;\n";
            stream << "}\n\n";
            stream << "void RemoveHooks() {\n";
            stream << "    DetourTransactionBegin();\n";
            stream << "    DetourUpdateThread(GetCurrentThread());\n";
            stream << "    DetourDetach(&(PVOID&)g_originalSleep, HookedSleep);\n";
            stream << "    DetourTransactionCommit();\n";
            stream << "}\n\n";
        } else {
            stream << "#include <MinHook.h>\n\n";
            stream << "static decltype(&Sleep) g_originalSleep = &Sleep;\n\n";
            stream << "VOID WINAPI HookedSleep(DWORD ms) {\n";
            stream << "    g_originalSleep(ms);\n";
            stream << "}\n\n";
            stream << "bool InstallHooks() {\n";
            stream << "    if (MH_Initialize() != MH_OK) return false;\n";
            stream << "    if (MH_CreateHook(&Sleep, &HookedSleep, reinterpret_cast<LPVOID*>(&g_originalSleep)) != MH_OK) return false;\n";
            stream << "    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;\n";
            stream << "}\n\n";
            stream << "void RemoveHooks() {\n";
            stream << "    MH_DisableHook(MH_ALL_HOOKS);\n";
            stream << "    MH_Uninitialize();\n";
            stream << "}\n\n";
        }

        stream << "BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {\n";
        stream << "    if (reason == DLL_PROCESS_ATTACH) {\n";
        stream << "        DisableThreadLibraryCalls(module);\n";
        stream << "        InstallHooks();\n";
        stream << "    } else if (reason == DLL_PROCESS_DETACH) {\n";
        stream << "        RemoveHooks();\n";
        stream << "    }\n";
        stream << "    return TRUE;\n";
        stream << "}\n";

        std::filesystem::path notesPath = outputDir / L"build_notes.txt";
        std::ofstream notes(notesPath, std::ios::binary | std::ios::trunc);
        if (notes.good()) {
            notes << "Backend: " << WideToUtf8(backend) << "\n";
            notes << "1) Add include/lib paths for " << WideToUtf8(backend) << ".\n";
            notes << "2) Build as x64 DLL.\n";
            notes << "3) Set built DLL path in RipperForge Hook tab and click Inject Hook DLL.\n";
            if (_wcsicmp(backend.c_str(), L"Detours") == 0) {
                notes << "Expected headers/libs: detours.h + detours.lib\n";
            } else {
                notes << "Expected headers/libs: MinHook.h + libMinHook.x64.lib\n";
            }
        }

        settings_.hookBackend = backend;
        settings_.hookDllPath = (outputDir / (std::wstring(engine) + L"Hook.dll")).wstring();
        SetWindowTextW(hookDllEdit_, settings_.hookDllPath.c_str());
        core::Logger::Instance().Info("Hook template created (" + WideToUtf8(backend) + "): " + WideToUtf8(outputPath.wstring()));
        core::Logger::Instance().Info("Build notes: " + WideToUtf8(notesPath.wstring()));
    }

    void RefreshPluginList() {
        SendMessageW(pluginList_, LB_RESETCONTENT, 0, 0);

        const auto& plugins = pluginManager_.Plugins();
        for (const auto& plugin : plugins) {
            const std::wstring line = Utf8ToWide(plugin.name) + L"  (" + plugin.filePath + L")";
            SendMessageW(pluginList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }

        if (plugins.empty()) {
            SendMessageW(pluginList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No plugins loaded."));
        }

        core::Logger::Instance().Info("Plugin catalog refreshed.");
    }

    void AppendLog(const std::wstring& line) {
        if (logEdit_ == nullptr) {
            pendingLogs_.push_back(line);
            return;
        }

        for (const auto& pending : pendingLogs_) {
            AppendLogLine(pending);
        }
        pendingLogs_.clear();

        AppendLogLine(line);
    }

    void AppendLogLine(const std::wstring& line) {
        const std::wstring withBreak = line + L"\r\n";
        const int length = GetWindowTextLengthW(logEdit_);
        SendMessageW(logEdit_, EM_SETSEL, length, length);
        SendMessageW(logEdit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(withBreak.c_str()));
    }

    void ApplyFontRecursively(HWND parent) {
        if (uiFont_ == nullptr) {
            return;
        }

        SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);

        HWND child = GetWindow(parent, GW_CHILD);
        while (child != nullptr) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }

private:
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;

    HBRUSH darkBrush_ = CreateSolidBrush(RGB(20, 20, 24));
    HBRUSH inputBrush_ = CreateSolidBrush(RGB(28, 28, 34));
    HFONT uiFont_ = nullptr;

    HWND searchEdit_ = nullptr;
    HWND refreshButton_ = nullptr;
    HWND autoRefreshCheckbox_ = nullptr;
    HWND elevateButton_ = nullptr;
    HWND processList_ = nullptr;
    HWND tabControl_ = nullptr;

    std::array<HWND, static_cast<size_t>(TabIndex::Count)> pageWindows_{};

    HWND dllPathEdit_ = nullptr;

    HWND memAddressEdit_ = nullptr;
    HWND memBytesEdit_ = nullptr;
    HWND memPatternEdit_ = nullptr;
    HWND memResultsList_ = nullptr;

    HWND captureDllEdit_ = nullptr;
    HWND captureOutputEdit_ = nullptr;
    HWND captureProgress_ = nullptr;
    HWND assetTextureList_ = nullptr;
    HWND assetModelList_ = nullptr;
    HWND assetPreviewHost_ = nullptr;

    HWND hookEngineCombo_ = nullptr;
    HWND hookDllEdit_ = nullptr;
    HWND hookBackendCombo_ = nullptr;

    HWND pluginList_ = nullptr;
    HWND logEdit_ = nullptr;

    bool captureRunning_ = false;
    bool previewInitialized_ = false;
    uint32_t lastCaptureAssetCount_ = 0;

    std::wstring configPath_;
    std::wstring pluginDir_;

    core::AppSettings settings_;
    core::AssetRipperBridge assetBridge_;
    core::Dx11PreviewRenderer previewRenderer_;
    core::TextureData currentTexture_;
    core::MeshData currentMesh_;
    std::vector<std::filesystem::path> capturedTextureAssets_;
    std::vector<std::filesystem::path> capturedModelAssets_;
    std::vector<core::ProcessInfo> processes_;
    std::vector<std::wstring> pendingLogs_;
    plugins::PluginManager pluginManager_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    MainWindow window;
    if (!window.Create(instance)) {
        MessageBoxW(nullptr, L"Failed to start RipperForge.", L"RipperForge", MB_ICONERROR | MB_OK);
        return 1;
    }

    return window.Run();
}
