#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <cstdio>

// Simple injector UI and logic for AssetRipper

struct ProcessEntry {
    DWORD pid{};
    std::wstring name;
};

static const wchar_t* kClassName = L"AssetRipperInjectorWindow";
static const wchar_t* kStatusReady = L"Status: Ready";
static const wchar_t* kStatusInjecting = L"Status: Injecting...";
static const wchar_t* kStatusFailed = L"Status: Failed";
static const wchar_t* kStatusDone = L"Status: Injected";

enum ControlIds {
    IDC_PROCESS_LIST = 101,
    IDC_REFRESH      = 102,
    IDC_INJECT       = 103,
    IDC_DLL_PATH     = 104,
    IDC_BROWSE_DLL   = 105,
    IDC_OUT_PATH     = 106,
    IDC_BROWSE_OUT   = 107,
    IDC_STATUS       = 108
};

class InjectorApp {
public:
    int Run(HINSTANCE hInstance, int nCmdShow);

private:
    HWND m_hWnd{};
    HWND m_processList{};
    HWND m_status{};
    HWND m_dllPath{};
    HWND m_outPath{};
    std::vector<ProcessEntry> m_processes;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void CreateUI(HWND hWnd);
    void RefreshProcesses();
    void SetStatus(const wchar_t* text);
    void OnInject();
    void BrowseDll();
    void BrowseOutput();
    std::wstring GetText(HWND ctrl);
    void WriteConfigFile(const std::wstring& outputDir, DWORD pid, bool autoCapture = false,
                         bool overlay = true, bool captureFrame = false);
    bool InjectIntoProcess(DWORD pid, const std::wstring& dllPath);
};

static std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                        out.data(), sizeNeeded, nullptr, nullptr);
    return out;
}

static std::wstring GetLastErrorMessage(DWORD err) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring msg = size && buffer ? buffer : L"Unknown error";
    if (buffer) LocalFree(buffer);
    return msg;
}

static bool InjectIntoProcessHandle(HANDLE hProcess, const std::wstring& dllPath, std::wstring* error) {
    if (!hProcess) {
        if (error) *error = L"Invalid process handle";
        return false;
    }
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) {
        if (error) *error = L"VirtualAllocEx failed: " + GetLastErrorMessage(GetLastError());
        return false;
    }
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), bytes, nullptr)) {
        if (error) *error = L"WriteProcessMemory failed: " + GetLastErrorMessage(GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                        reinterpret_cast<LPTHREAD_START_ROUTINE>(LoadLibraryW),
                                        remoteMem, 0, nullptr);
    if (!hThread) {
        if (error) *error = L"CreateRemoteThread failed: " + GetLastErrorMessage(GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    if (exitCode == 0) {
        if (error) *error = L"LoadLibraryW returned NULL in remote process";
        return false;
    }
    return true;
}

static bool InjectIntoProcessByPid(DWORD pid, const std::wstring& dllPath, std::wstring* error) {
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
    if (!hProcess) {
        if (error) *error = L"OpenProcess failed: " + GetLastErrorMessage(GetLastError());
        return false;
    }
    const bool ok = InjectIntoProcessHandle(hProcess, dllPath, error);
    CloseHandle(hProcess);
    return ok;
}

static void WriteConfigFile(const std::wstring& outputDir, DWORD pid, bool autoCapture,
                            bool overlay, bool captureFrame) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstringstream ss;
    ss << L"asset_ripper_" << pid << L".cfg";
    std::filesystem::path cfg = std::filesystem::path(tempPath) / ss.str();
    std::ofstream out(cfg, std::ios::binary);
    if (!out.is_open()) return;
    std::string utf8Path = WideToUtf8(outputDir);
    out << "output_dir=" << utf8Path << "\n";
    out << "auto_capture=" << (autoCapture ? "1" : "0") << "\n";
    out << "overlay=" << (overlay ? "1" : "0") << "\n";
    out << "capture_frame=" << (captureFrame ? "1" : "0") << "\n";
    out << "gltf_flip_z=1\n";
    out << "gltf_flip_winding=1\n";
    out << "gltf_flip_v=0\n";
    out << "gltf_flip_normal_green=0\n";
    out << "gltf_flip_tangent_w=0\n";
    out << "gltf_dedup_meshes=1\n";
    out << "gltf_dedup_textures=1\n";
    out << "gltf_dedup_samplers=1\n";
}

struct LaunchOptions {
    bool enabled{false};
    bool wait{false};
    bool autoCapture{false};
    bool overlay{false};
    bool captureFrame{false};
    std::wstring exePath;
    std::wstring dllPath;
    std::wstring outDir;
    std::wstring args;
    std::wstring workDir;
};

static std::filesystem::path GetModuleDir() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

static std::wstring QuoteIfNeeded(const std::wstring& input) {
    if (input.find_first_of(L" \t") == std::wstring::npos) return input;
    return L"\"" + input + L"\"";
}

static bool ParseLaunchOptions(int argc, wchar_t** argv, LaunchOptions& out, std::wstring& error) {
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--launch" && i + 1 < argc) {
            out.enabled = true;
            out.exePath = argv[++i];
        } else if (arg == L"--dll" && i + 1 < argc) {
            out.dllPath = argv[++i];
        } else if (arg == L"--out" && i + 1 < argc) {
            out.outDir = argv[++i];
        } else if (arg == L"--args" && i + 1 < argc) {
            out.args = argv[++i];
        } else if (arg == L"--cwd" && i + 1 < argc) {
            out.workDir = argv[++i];
        } else if (arg == L"--wait") {
            out.wait = true;
        } else if (arg == L"--auto-capture") {
            out.autoCapture = true;
        } else if (arg == L"--capture-frame") {
            out.captureFrame = true;
        } else if (arg == L"--overlay") {
            out.overlay = true;
        } else if (arg == L"--no-overlay") {
            out.overlay = false;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            out.enabled = true;
            error = L"help";
            return false;
        }
    }

    if (!out.enabled) return false;
    if (out.exePath.empty()) {
        error = L"--launch requires a path to the game exe";
        return false;
    }

    if (out.dllPath.empty()) {
        out.dllPath = (GetModuleDir() / "ripper_new6.dll").wstring();
    }
    if (out.outDir.empty()) {
        out.outDir = (GetModuleDir() / "captures").wstring();
    }
    if (out.workDir.empty()) {
        out.workDir = std::filesystem::path(out.exePath).parent_path().wstring();
    }

    if (GetFileAttributesW(out.exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Launch exe not found";
        return false;
    }
    if (GetFileAttributesW(out.dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"DLL not found";
        return false;
    }
    return true;
}

static void PrintUsage() {
    fwprintf(stdout,
             L"Usage:\n"
             L"  AssetRipper.exe --launch <exe> [--dll <dll>] [--out <dir>] [--args \"...\"]\n"
             L"                  [--cwd <dir>] [--auto-capture] [--capture-frame]\n"
             L"                  [--overlay|--no-overlay] [--wait]\n");
}

static int RunLaunchMode(const LaunchOptions& opt) {
    std::error_code ec;
    std::filesystem::create_directories(opt.outDir, ec);

    std::wstring cmdLine = QuoteIfNeeded(opt.exePath);
    if (!opt.args.empty()) {
        cmdLine += L" ";
        cmdLine += opt.args;
    }

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(opt.exePath.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, opt.workDir.c_str(), &si, &pi)) {
        std::wstring err = GetLastErrorMessage(GetLastError());
        fwprintf(stderr, L"CreateProcess failed: %s\n", err.c_str());
        return 1;
    }

    WriteConfigFile(opt.outDir, pi.dwProcessId, opt.autoCapture, opt.overlay, opt.captureFrame);

    std::wstring injectErr;
    if (!InjectIntoProcessHandle(pi.hProcess, opt.dllPath, &injectErr)) {
        fwprintf(stderr, L"Injection failed: %s\n", injectErr.c_str());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 2;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    if (opt.wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    CloseHandle(pi.hProcess);
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    LaunchOptions opts{};
    std::wstring error;
    if (argv) {
        if (ParseLaunchOptions(argc, argv, opts, error)) {
            LocalFree(argv);
            return RunLaunchMode(opts);
        }
        if (opts.enabled) {
            if (error == L"help") {
                PrintUsage();
                LocalFree(argv);
                return 0;
            }
            fwprintf(stderr, L"%s\n", error.c_str());
            PrintUsage();
            LocalFree(argv);
            return 1;
        }
        LocalFree(argv);
    }
    InjectorApp app;
    return app.Run(hInstance, nCmdShow);
}

int InjectorApp::Run(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = InjectorApp::WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszClassName = kClassName;
    RegisterClassExW(&wcex);

    m_hWnd = CreateWindowW(kClassName, L"AssetRipper Injector",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, 0, 640, 420, nullptr, nullptr, hInstance, this);
    if (!m_hWnd) {
        MessageBoxW(nullptr, L"Failed to create main window", L"AssetRipper", MB_ICONERROR);
        return -1;
    }

    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK InjectorApp::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    InjectorApp* self = nullptr;
    if (message == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<InjectorApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<InjectorApp*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (!self) {
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE:
        self->CreateUI(hWnd);
        self->RefreshProcesses();
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        switch (id) {
        case IDC_REFRESH:
            self->RefreshProcesses();
            break;
        case IDC_INJECT:
            self->OnInject();
            break;
        case IDC_BROWSE_DLL:
            self->BrowseDll();
            break;
        case IDC_BROWSE_OUT:
            self->BrowseOutput();
            break;
        default:
            break;
        }
    } break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

void InjectorApp::CreateUI(HWND hWnd) {
    // Labels and inputs
    CreateWindowW(L"STATIC", L"Processes:", WS_VISIBLE | WS_CHILD,
                  10, 10, 100, 20, hWnd, nullptr, nullptr, nullptr);
    m_processList = CreateWindowW(L"LISTBOX", nullptr,
                                  WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_STANDARD,
                                  10, 30, 400, 250, hWnd,
                                  reinterpret_cast<HMENU>(IDC_PROCESS_LIST), nullptr, nullptr);

    CreateWindowW(L"BUTTON", L"Refresh", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  420, 30, 180, 30, hWnd,
                  reinterpret_cast<HMENU>(IDC_REFRESH), nullptr, nullptr);

    CreateWindowW(L"STATIC", L"DLL Path:", WS_VISIBLE | WS_CHILD,
                  10, 290, 80, 20, hWnd, nullptr, nullptr, nullptr);
    m_dllPath = CreateWindowW(L"EDIT", L"",
                              WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                              90, 285, 380, 24, hWnd,
                              reinterpret_cast<HMENU>(IDC_DLL_PATH), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  480, 285, 40, 24, hWnd,
                  reinterpret_cast<HMENU>(IDC_BROWSE_DLL), nullptr, nullptr);

    CreateWindowW(L"STATIC", L"Output Dir:", WS_VISIBLE | WS_CHILD,
                  10, 320, 80, 20, hWnd, nullptr, nullptr, nullptr);
    m_outPath = CreateWindowW(L"EDIT", L"",
                              WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                              90, 315, 380, 24, hWnd,
                              reinterpret_cast<HMENU>(IDC_OUT_PATH), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  480, 315, 40, 24, hWnd,
                  reinterpret_cast<HMENU>(IDC_BROWSE_OUT), nullptr, nullptr);

    CreateWindowW(L"BUTTON", L"Inject DLL", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  420, 70, 180, 40, hWnd,
                  reinterpret_cast<HMENU>(IDC_INJECT), nullptr, nullptr);

    m_status = CreateWindowW(L"STATIC", kStatusReady, WS_VISIBLE | WS_CHILD,
                             10, 350, 580, 20, hWnd,
                             reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path base(exePath);
    auto dllDefault = (base.parent_path() / "ripper_new6.dll").wstring();
    SetWindowTextW(m_dllPath, dllDefault.c_str());

    auto outDefault = (base.parent_path() / "captures").wstring();
    SetWindowTextW(m_outPath, outDefault.c_str());
}

std::wstring InjectorApp::GetText(HWND ctrl) {
    const int len = GetWindowTextLengthW(ctrl);
    if (len <= 0) return L"";
    std::wstring text(len, L'\0');
    GetWindowTextW(ctrl, text.data(), len + 1);
    return text;
}

void InjectorApp::SetStatus(const wchar_t* text) {
    SetWindowTextW(m_status, text);
}

void InjectorApp::RefreshProcesses() {
    SendMessageW(m_processList, LB_RESETCONTENT, 0, 0);
    m_processes.clear();

    DWORD processes[2048]{};
    DWORD bytesReturned = 0;
    if (!EnumProcesses(processes, sizeof(processes), &bytesReturned)) {
        MessageBoxW(m_hWnd, L"EnumProcesses failed", L"AssetRipper", MB_ICONERROR);
        return;
    }
    const size_t count = bytesReturned / sizeof(DWORD);
    for (size_t i = 0; i < count; ++i) {
        DWORD pid = processes[i];
        if (pid == 0) continue;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) continue;

        wchar_t name[MAX_PATH] = L"<unknown>";
        HMODULE hMod = nullptr;
        DWORD needed = 0;
        if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &needed)) {
            GetModuleBaseNameW(hProcess, hMod, name, MAX_PATH);
        }
        CloseHandle(hProcess);

        ProcessEntry entry{pid, name};
        m_processes.push_back(entry);
        std::wstringstream ss;
        ss << name << L" (PID " << pid << L")";
        SendMessageW(m_processList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ss.str().c_str()));
    }
}

void InjectorApp::BrowseDll() {
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"DLL Files (*.dll)\0*.dll\0All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(m_dllPath, file);
    }
}

void InjectorApp::BrowseOutput() {
    BROWSEINFOW bi{};
    bi.hwndOwner = m_hWnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"Select output directory";
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(m_outPath, path);
        }
        CoTaskMemFree(pidl);
    }
}

void InjectorApp::WriteConfigFile(const std::wstring& outputDir, DWORD pid, bool autoCapture,
                                  bool overlay, bool captureFrame) {
    ::WriteConfigFile(outputDir, pid, autoCapture, overlay, captureFrame);
}

bool InjectorApp::InjectIntoProcess(DWORD pid, const std::wstring& dllPath) {
    SetStatus(kStatusInjecting);
    std::wstring error;
    if (!InjectIntoProcessByPid(pid, dllPath, &error)) {
        MessageBoxW(m_hWnd, error.c_str(), L"AssetRipper", MB_ICONERROR);
        SetStatus(kStatusFailed);
        return false;
    }
    SetStatus(kStatusDone);
    return true;
}

void InjectorApp::OnInject() {
    const int sel = static_cast<int>(SendMessageW(m_processList, LB_GETCURSEL, 0, 0));
    if (sel == LB_ERR || sel >= static_cast<int>(m_processes.size())) {
        MessageBoxW(m_hWnd, L"Select a process first", L"AssetRipper", MB_ICONINFORMATION);
        return;
    }

    std::wstring dll = GetText(m_dllPath);
    std::wstring outDir = GetText(m_outPath);
    if (dll.empty() || GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(m_hWnd, L"Invalid DLL path", L"AssetRipper", MB_ICONERROR);
        return;
    }
    if (!std::filesystem::exists(outDir)) {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
    }

    const DWORD pid = m_processes[sel].pid;
    WriteConfigFile(outDir, pid);
    InjectIntoProcess(pid, dll);
}
