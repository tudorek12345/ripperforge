#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

#include "core/AssetIO.h"

namespace rf::core {

enum class PreviewMode {
    Texture = 0,
    Model = 1,
};

class Dx11PreviewRenderer {
public:
    Dx11PreviewRenderer();
    ~Dx11PreviewRenderer();

    bool Initialize(HWND hostWindow, std::string& error);
    void Shutdown();

    void Resize();
    void Render(float deltaTimeSeconds);

    bool SetTexture(const TextureData& texture, std::string& error);
    bool SetMesh(const MeshData& mesh, std::string& error);

    void SetMode(PreviewMode mode);
    PreviewMode Mode() const;

private:
    bool CreateDeviceAndSwapChain(std::string& error);
    bool CreateRenderTargets(std::string& error);
    bool CreateShaders(std::string& error);
    bool CreatePipelineState(std::string& error);
    bool CreateStaticGeometry(std::string& error);

    bool UpdateTextureResource(const TextureData& texture, std::string& error);
    bool UpdateMeshBuffers(const MeshData& mesh, std::string& error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    PreviewMode mode_ = PreviewMode::Texture;
};

} // namespace rf::core
