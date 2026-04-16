#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rf::core {

struct CapturePollResult {
    float progress01 = 0.0f;
    uint32_t textureCount = 0;
    uint32_t modelCount = 0;
    uint32_t totalCount = 0;
};

class AssetRipperBridge {
public:
    AssetRipperBridge();
    ~AssetRipperBridge();

    bool Initialize(const std::wstring& moduleDirectory);

    void SetOutputDirectory(std::wstring outputDirectory);
    void SetCaptureDllPath(std::wstring captureDllPath);

    const std::wstring& OutputDirectory() const;
    const std::wstring& CaptureDllPath() const;

    bool StartCapture(DWORD pid, std::string& error);
    void StopCapture();
    bool IsCaptureRunning() const;
    float QueryCaptureProgress() const;

    CapturePollResult Poll();

    std::vector<std::filesystem::path> EnumerateTextureAssets() const;
    std::vector<std::filesystem::path> EnumerateModelAssets() const;

private:
    struct BridgeApi;

    bool TryLoadBridgeDll(const std::filesystem::path& moduleDirectory);
    std::filesystem::path ResolveDefaultCaptureDll(const std::filesystem::path& moduleDirectory) const;

    CapturePollResult ScanOutputDirectory() const;

private:
    std::filesystem::path moduleDirectory_;
    std::wstring outputDirectory_;
    std::wstring captureDllPath_;

    bool captureRunning_ = false;
    bool bridgeActive_ = false;

    HMODULE bridgeModule_ = nullptr;
    BridgeApi* bridgeApi_ = nullptr;
};

} // namespace rf::core
