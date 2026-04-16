#include "core/AssetRipperBridge.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Injector.h"
#include "core/Logger.h"

namespace rf::core {

namespace {

using InitFn = bool(__stdcall*)(const wchar_t* outputDirectory);
using StartFn = bool(__stdcall*)(DWORD pid, const wchar_t* captureDllPath, const wchar_t* outputDirectory);
using StopFn = void(__stdcall*)();
using ProgressFn = float(__stdcall*)();

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

bool HasAnyExtension(const std::filesystem::path& path, const std::array<const wchar_t*, 8>& extensions) {
    const std::wstring ext = ToLower(path.extension().wstring());
    for (const auto* candidate : extensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

void WriteAssetRipperConfig(const std::wstring& outputDir, DWORD pid) {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);

    std::wstringstream fileName;
    fileName << L"asset_ripper_" << pid << L".cfg";

    std::filesystem::path cfgPath = std::filesystem::path(tempPath) / fileName.str();
    std::ofstream cfg(cfgPath, std::ios::binary | std::ios::trunc);
    if (!cfg.good()) {
        return;
    }

    const int utf8Size = WideCharToMultiByte(CP_UTF8, 0, outputDir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Dir;
    if (utf8Size > 1) {
        utf8Dir.resize(static_cast<size_t>(utf8Size - 1));
        WideCharToMultiByte(CP_UTF8, 0, outputDir.c_str(), -1, utf8Dir.data(), utf8Size, nullptr, nullptr);
    }

    cfg << "output_dir=" << utf8Dir << "\n";
    cfg << "auto_capture=0\n";
    cfg << "overlay=1\n";
    cfg << "capture_frame=1\n";
    cfg << "gltf_flip_z=1\n";
    cfg << "gltf_flip_winding=1\n";
    cfg << "gltf_flip_v=0\n";
    cfg << "gltf_flip_normal_green=0\n";
    cfg << "gltf_flip_tangent_w=0\n";
    cfg << "gltf_dedup_meshes=1\n";
    cfg << "gltf_dedup_textures=1\n";
    cfg << "gltf_dedup_samplers=1\n";
}

} // namespace

struct AssetRipperBridge::BridgeApi {
    InitFn initialize = nullptr;
    StartFn startCapture = nullptr;
    StopFn stopCapture = nullptr;
    ProgressFn getProgress = nullptr;
};

AssetRipperBridge::AssetRipperBridge() {
    bridgeApi_ = new BridgeApi();
}

AssetRipperBridge::~AssetRipperBridge() {
    StopCapture();

    if (bridgeModule_ != nullptr) {
        FreeLibrary(bridgeModule_);
        bridgeModule_ = nullptr;
    }

    delete bridgeApi_;
    bridgeApi_ = nullptr;
}

bool AssetRipperBridge::Initialize(const std::wstring& moduleDirectory) {
    moduleDirectory_ = std::filesystem::path(moduleDirectory);

    std::error_code ec;
    std::filesystem::create_directories(moduleDirectory_ / L"captures", ec);

    outputDirectory_ = (moduleDirectory_ / L"captures").wstring();
    captureDllPath_ = ResolveDefaultCaptureDll(moduleDirectory_).wstring();

    TryLoadBridgeDll(moduleDirectory_);
    return true;
}

void AssetRipperBridge::SetOutputDirectory(std::wstring outputDirectory) {
    outputDirectory_ = std::move(outputDirectory);
}

void AssetRipperBridge::SetCaptureDllPath(std::wstring captureDllPath) {
    captureDllPath_ = std::move(captureDllPath);
}

const std::wstring& AssetRipperBridge::OutputDirectory() const {
    return outputDirectory_;
}

const std::wstring& AssetRipperBridge::CaptureDllPath() const {
    return captureDllPath_;
}

bool AssetRipperBridge::StartCapture(DWORD pid, std::string& error) {
    if (pid == 0) {
        error = "No target process selected.";
        return false;
    }

    if (captureDllPath_.empty()) {
        error = "Capture DLL path is empty.";
        return false;
    }

    std::filesystem::path resolvedDllPath = std::filesystem::path(captureDllPath_);
    if (!resolvedDllPath.is_absolute()) {
        resolvedDllPath = moduleDirectory_ / resolvedDllPath;
    }

    if (!std::filesystem::exists(resolvedDllPath)) {
        error = "Capture DLL was not found.";
        return false;
    }
    captureDllPath_ = resolvedDllPath.wstring();

    std::error_code ec;
    std::filesystem::create_directories(outputDirectory_, ec);
    if (ec) {
        error = "Could not create capture output directory.";
        return false;
    }

    WriteAssetRipperConfig(outputDirectory_, pid);

    if (bridgeActive_ && bridgeApi_ != nullptr && bridgeApi_->startCapture != nullptr) {
        if (bridgeApi_->initialize != nullptr) {
            bridgeApi_->initialize(outputDirectory_.c_str());
        }

        if (!bridgeApi_->startCapture(pid, captureDllPath_.c_str(), outputDirectory_.c_str())) {
            error = "AssetRipper bridge start call failed.";
            return false;
        }

        captureRunning_ = true;
        Logger::Instance().Info("AssetRipper bridge capture started.");
        return true;
    }

    if (!InjectDll(pid, captureDllPath_, error)) {
        return false;
    }

    captureRunning_ = true;
    Logger::Instance().Info("Capture DLL injected with AssetRipper config file.");
    return true;
}

void AssetRipperBridge::StopCapture() {
    if (!captureRunning_) {
        return;
    }

    if (bridgeActive_ && bridgeApi_ != nullptr && bridgeApi_->stopCapture != nullptr) {
        bridgeApi_->stopCapture();
    }

    captureRunning_ = false;
}

bool AssetRipperBridge::IsCaptureRunning() const {
    return captureRunning_;
}

float AssetRipperBridge::QueryCaptureProgress() const {
    if (!bridgeActive_ || bridgeApi_ == nullptr || bridgeApi_->getProgress == nullptr) {
        return -1.0f;
    }

    const float progress = bridgeApi_->getProgress();
    if (progress < 0.0f || progress > 1.0f) {
        return -1.0f;
    }

    return progress;
}

CapturePollResult AssetRipperBridge::Poll() {
    CapturePollResult result = ScanOutputDirectory();

    const float bridgeProgress = QueryCaptureProgress();
    if (bridgeProgress >= 0.0f) {
        result.progress01 = bridgeProgress;
        return result;
    }

    if (!captureRunning_) {
        result.progress01 = 0.0f;
        return result;
    }

    result.progress01 = std::min(0.95f, static_cast<float>(result.totalCount) / 100.0f);
    return result;
}

std::vector<std::filesystem::path> AssetRipperBridge::EnumerateTextureAssets() const {
    std::vector<std::filesystem::path> files;

    std::error_code ec;
    if (!std::filesystem::exists(outputDirectory_, ec)) {
        return files;
    }

    constexpr std::array<const wchar_t*, 8> kTextureExtensions = {
        L".dds",
        L".png",
        L".jpg",
        L".jpeg",
        L".bmp",
        L".tif",
        L".tiff",
        L".gif",
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator(outputDirectory_, ec)) {
        if (ec) {
            break;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        if (HasAnyExtension(entry.path(), kTextureExtensions)) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::filesystem::path> AssetRipperBridge::EnumerateModelAssets() const {
    std::vector<std::filesystem::path> files;

    std::error_code ec;
    if (!std::filesystem::exists(outputDirectory_, ec)) {
        return files;
    }

    constexpr std::array<const wchar_t*, 8> kModelExtensions = {
        L".obj",
        L".fbx",
        L".glb",
        L".gltf",
        L".ply",
        L".stl",
        L".dae",
        L".x",
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator(outputDirectory_, ec)) {
        if (ec) {
            break;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        if (HasAnyExtension(entry.path(), kModelExtensions)) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

bool AssetRipperBridge::TryLoadBridgeDll(const std::filesystem::path& moduleDirectory) {
    if (bridgeModule_ != nullptr) {
        FreeLibrary(bridgeModule_);
        bridgeModule_ = nullptr;
    }

    bridgeActive_ = false;
    if (bridgeApi_ == nullptr) {
        return false;
    }

    bridgeApi_->initialize = nullptr;
    bridgeApi_->startCapture = nullptr;
    bridgeApi_->stopCapture = nullptr;
    bridgeApi_->getProgress = nullptr;

    constexpr std::array<const wchar_t*, 3> kCandidates = {
        L"AssetRIpperBridge.dll",
        L"AssetRipperBridge.dll",
        L"ripper_bridge.dll",
    };

    for (const auto* candidate : kCandidates) {
        const std::filesystem::path path = moduleDirectory / candidate;
        if (!std::filesystem::exists(path)) {
            continue;
        }

        bridgeModule_ = LoadLibraryW(path.c_str());
        if (bridgeModule_ == nullptr) {
            continue;
        }

        bridgeApi_->initialize = reinterpret_cast<InitFn>(GetProcAddress(bridgeModule_, "RF_AR_Initialize"));
        bridgeApi_->startCapture = reinterpret_cast<StartFn>(GetProcAddress(bridgeModule_, "RF_AR_StartCapture"));
        bridgeApi_->stopCapture = reinterpret_cast<StopFn>(GetProcAddress(bridgeModule_, "RF_AR_StopCapture"));
        bridgeApi_->getProgress = reinterpret_cast<ProgressFn>(GetProcAddress(bridgeModule_, "RF_AR_GetProgress"));

        if (bridgeApi_->startCapture != nullptr) {
            bridgeActive_ = true;
            Logger::Instance().Info("AssetRipper bridge loaded: " + path.string());
            return true;
        }

        FreeLibrary(bridgeModule_);
        bridgeModule_ = nullptr;
    }

    return false;
}

std::filesystem::path AssetRipperBridge::ResolveDefaultCaptureDll(const std::filesystem::path& moduleDirectory) const {
    constexpr std::array<const wchar_t*, 8> kCandidates = {
        L"ripper_new6.dll",
        L"ripper.dll",
        L"capture.dll",
        L"AssetRipperCapture.dll",
        L"AssetRIpperCapture.dll",
        L"external\\AssetRIpper\\ripper_new6.dll",
        L"external\\AssetRIpper\\ripper.dll",
        L"external\\AssetRIpper\\build\\ripper_new6.dll",
    };

    for (const auto* candidate : kCandidates) {
        const std::filesystem::path path = moduleDirectory / candidate;
        if (std::filesystem::exists(path)) {
            return path;
        }
    }

    return moduleDirectory / L"ripper_new6.dll";
}

CapturePollResult AssetRipperBridge::ScanOutputDirectory() const {
    CapturePollResult result;

    const auto textures = EnumerateTextureAssets();
    const auto models = EnumerateModelAssets();

    result.textureCount = static_cast<uint32_t>(textures.size());
    result.modelCount = static_cast<uint32_t>(models.size());
    result.totalCount = result.textureCount + result.modelCount;
    return result;
}

} // namespace rf::core
