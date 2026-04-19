#include "core/Dx11PreviewRenderer.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace rf::core {

using Microsoft::WRL::ComPtr;

namespace {

struct SceneVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct SceneCBuffer {
    DirectX::XMFLOAT4X4 mvp{};
    DirectX::XMFLOAT4 params{};
};

std::string HrToString(HRESULT hr) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buffer);
}

constexpr const char* kVertexShaderSource = R"(
cbuffer SceneConstants : register(b0)
{
    float4x4 gMvp;
    float4 gParams;
}

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOut main(VSIn input)
{
    VSOut output;
    output.pos = mul(float4(input.pos, 1.0), gMvp);
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}
)";

constexpr const char* kPixelShaderSource = R"(
Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

cbuffer SceneConstants : register(b0)
{
    float4x4 gMvp;
    float4 gParams;
}

struct PSIn
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

float4 main(PSIn input) : SV_TARGET
{
    float4 tex = gTexture.Sample(gSampler, input.uv);
    if (gParams.x < 0.5) {
        return tex;
    }

    float3 n = normalize(input.normal);
    float3 l = normalize(float3(0.35, 0.8, -0.25));
    float ndotl = saturate(dot(n, l));
    float lighting = 0.25 + 0.75 * ndotl;
    return float4(tex.rgb * lighting, 1.0);
}
)";

} // namespace

struct Dx11PreviewRenderer::Impl {
    HWND hostWindow = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTargetView;
    ComPtr<ID3D11DepthStencilView> depthStencilView;
    ComPtr<ID3D11Texture2D> depthTexture;

    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constantBuffer;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11SamplerState> samplerState;

    ComPtr<ID3D11Buffer> quadVertexBuffer;
    ComPtr<ID3D11Buffer> quadIndexBuffer;
    uint32_t quadIndexCount = 0;

    ComPtr<ID3D11Buffer> modelVertexBuffer;
    ComPtr<ID3D11Buffer> modelIndexBuffer;
    uint32_t modelIndexCount = 0;

    ComPtr<ID3D11ShaderResourceView> textureSrv;

    float elapsedSeconds = 0.0f;
};

Dx11PreviewRenderer::Dx11PreviewRenderer() {
    impl_ = new Impl();
}

Dx11PreviewRenderer::~Dx11PreviewRenderer() {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool Dx11PreviewRenderer::Initialize(HWND hostWindow, std::string& error) {
    if (impl_ == nullptr) {
        error = "Renderer not allocated.";
        return false;
    }

    impl_->hostWindow = hostWindow;

    if (!CreateDeviceAndSwapChain(error)) {
        return false;
    }
    if (!CreateRenderTargets(error)) {
        return false;
    }
    if (!CreateShaders(error)) {
        return false;
    }
    if (!CreatePipelineState(error)) {
        return false;
    }
    if (!CreateStaticGeometry(error)) {
        return false;
    }

    std::string texError;
    const TextureData checker = BuildCheckerTexture(256, 256, 16);
    SetTexture(checker, texError);

    std::string meshError;
    const MeshData cube = BuildUnitCubeMesh();
    SetMesh(cube, meshError);

    return true;
}

void Dx11PreviewRenderer::Shutdown() {
    if (impl_ == nullptr) {
        return;
    }

    impl_->textureSrv.Reset();
    impl_->modelIndexBuffer.Reset();
    impl_->modelVertexBuffer.Reset();
    impl_->quadIndexBuffer.Reset();
    impl_->quadVertexBuffer.Reset();
    impl_->samplerState.Reset();
    impl_->rasterizerState.Reset();
    impl_->constantBuffer.Reset();
    impl_->inputLayout.Reset();
    impl_->pixelShader.Reset();
    impl_->vertexShader.Reset();
    impl_->depthStencilView.Reset();
    impl_->depthTexture.Reset();
    impl_->renderTargetView.Reset();
    impl_->swapChain.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
}

void Dx11PreviewRenderer::Resize() {
    if (impl_ == nullptr || impl_->swapChain == nullptr || impl_->hostWindow == nullptr) {
        return;
    }

    RECT rect{};
    GetClientRect(impl_->hostWindow, &rect);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->renderTargetView.Reset();
    impl_->depthStencilView.Reset();
    impl_->depthTexture.Reset();

    impl_->swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);

    std::string ignored;
    CreateRenderTargets(ignored);
}

void Dx11PreviewRenderer::Render(float deltaTimeSeconds) {
    if (impl_ == nullptr || impl_->context == nullptr || impl_->swapChain == nullptr || impl_->renderTargetView == nullptr) {
        return;
    }

    impl_->elapsedSeconds += deltaTimeSeconds;

    const float clearColor[4] = {0.09f, 0.09f, 0.12f, 1.0f};
    impl_->context->ClearRenderTargetView(impl_->renderTargetView.Get(), clearColor);
    if (impl_->depthStencilView != nullptr) {
        impl_->context->ClearDepthStencilView(impl_->depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    RECT rect{};
    GetClientRect(impl_->hostWindow, &rect);
    const float width = static_cast<float>(std::max(1L, rect.right - rect.left));
    const float height = static_cast<float>(std::max(1L, rect.bottom - rect.top));
    const float aspect = width / std::max(1.0f, height);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    impl_->context->RSSetViewports(1, &viewport);

    impl_->context->OMSetRenderTargets(1, impl_->renderTargetView.GetAddressOf(), impl_->depthStencilView.Get());

    impl_->context->IASetInputLayout(impl_->inputLayout.Get());
    impl_->context->VSSetShader(impl_->vertexShader.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->pixelShader.Get(), nullptr, 0);
    impl_->context->RSSetState(impl_->rasterizerState.Get());
    impl_->context->PSSetSamplers(0, 1, impl_->samplerState.GetAddressOf());
    if (impl_->textureSrv != nullptr) {
        impl_->context->PSSetShaderResources(0, 1, impl_->textureSrv.GetAddressOf());
    }

    SceneCBuffer constants{};
    DirectX::XMMATRIX mvp = DirectX::XMMatrixIdentity();
    float modeFlag = 0.0f;

    ID3D11Buffer* vertexBuffer = impl_->quadVertexBuffer.Get();
    ID3D11Buffer* indexBuffer = impl_->quadIndexBuffer.Get();
    uint32_t indexCount = impl_->quadIndexCount;

    if (mode_ == PreviewMode::Model && impl_->modelVertexBuffer != nullptr && impl_->modelIndexBuffer != nullptr) {
        modeFlag = 1.0f;
        vertexBuffer = impl_->modelVertexBuffer.Get();
        indexBuffer = impl_->modelIndexBuffer.Get();
        indexCount = impl_->modelIndexCount;

        const DirectX::XMMATRIX model =
            DirectX::XMMatrixRotationY(impl_->elapsedSeconds * 0.9f) *
            DirectX::XMMatrixRotationX(-0.28f);
        const DirectX::XMMATRIX view =
            DirectX::XMMatrixLookAtLH(
                DirectX::XMVectorSet(0.0f, 1.0f, -4.0f, 1.0f),
                DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(65.0f), aspect, 0.01f, 100.0f);
        mvp = DirectX::XMMatrixTranspose(model * view * proj);
    } else {
        const DirectX::XMMATRIX proj = DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity());
        mvp = proj;
    }

    DirectX::XMStoreFloat4x4(&constants.mvp, mvp);
    constants.params = DirectX::XMFLOAT4(modeFlag, impl_->elapsedSeconds, 0.0f, 0.0f);

    impl_->context->UpdateSubresource(impl_->constantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    impl_->context->VSSetConstantBuffers(0, 1, impl_->constantBuffer.GetAddressOf());
    impl_->context->PSSetConstantBuffers(0, 1, impl_->constantBuffer.GetAddressOf());

    const UINT stride = sizeof(SceneVertex);
    const UINT offset = 0;
    impl_->context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    impl_->context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->DrawIndexed(indexCount, 0, 0);

    impl_->swapChain->Present(1, 0);
}

bool Dx11PreviewRenderer::SetTexture(const TextureData& texture, std::string& error) {
    return UpdateTextureResource(texture, error);
}

bool Dx11PreviewRenderer::SetMesh(const MeshData& mesh, std::string& error) {
    return UpdateMeshBuffers(mesh, error);
}

void Dx11PreviewRenderer::SetMode(PreviewMode mode) {
    mode_ = mode;
}

PreviewMode Dx11PreviewRenderer::Mode() const {
    return mode_;
}

bool Dx11PreviewRenderer::CreateDeviceAndSwapChain(std::string& error) {
    RECT rect{};
    GetClientRect(impl_->hostWindow, &rect);
    const UINT width = static_cast<UINT>(std::max(1L, rect.right - rect.left));
    const UINT height = static_cast<UINT>(std::max(1L, rect.bottom - rect.top));

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = width;
    swapDesc.BufferDesc.Height = height;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.OutputWindow = impl_->hostWindow;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT creationFlags = 0;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL featureLevelOut = D3D_FEATURE_LEVEL_11_0;

    const D3D_FEATURE_LEVEL fallbackLevels[] = {D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    auto tryCreate = [&](D3D_DRIVER_TYPE driverType, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount) {
        impl_->swapChain.Reset();
        impl_->device.Reset();
        impl_->context.Reset();

        return D3D11CreateDeviceAndSwapChain(
            nullptr,
            driverType,
            nullptr,
            flags,
            levels,
            levelCount,
            D3D11_SDK_VERSION,
            &swapDesc,
            impl_->swapChain.GetAddressOf(),
            impl_->device.GetAddressOf(),
            &featureLevelOut,
            impl_->context.GetAddressOf());
    };

    HRESULT hr = tryCreate(
        D3D_DRIVER_TYPE_HARDWARE,
        creationFlags,
        featureLevels,
        static_cast<UINT>(std::size(featureLevels)));

    // Many systems do not have the D3D debug layer installed; retry without it.
    if (FAILED(hr) && (creationFlags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        hr = tryCreate(
            D3D_DRIVER_TYPE_HARDWARE,
            creationFlags & ~D3D11_CREATE_DEVICE_DEBUG,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)));
    }

    if (FAILED(hr)) {
        hr = tryCreate(
            D3D_DRIVER_TYPE_HARDWARE,
            creationFlags & ~D3D11_CREATE_DEVICE_DEBUG,
            fallbackLevels,
            static_cast<UINT>(std::size(fallbackLevels)));
    }

    // Final fallback for environments where hardware device creation is blocked.
    if (FAILED(hr)) {
        hr = tryCreate(
            D3D_DRIVER_TYPE_WARP,
            creationFlags & ~D3D11_CREATE_DEVICE_DEBUG,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)));
    }

    if (FAILED(hr)) {
        hr = tryCreate(
            D3D_DRIVER_TYPE_WARP,
            creationFlags & ~D3D11_CREATE_DEVICE_DEBUG,
            fallbackLevels,
            static_cast<UINT>(std::size(fallbackLevels)));
    }

    if (FAILED(hr)) {
        error = "D3D11CreateDeviceAndSwapChain failed after hardware+WARP fallback: " + HrToString(hr);
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            error += " (D3D debug runtime missing; install Graphics Tools or run without debug layer)";
        }
        return false;
    }

    return true;
}

bool Dx11PreviewRenderer::CreateRenderTargets(std::string& error) {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = impl_->swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        error = "Swap chain back buffer acquisition failed: " + HrToString(hr);
        return false;
    }

    hr = impl_->device->CreateRenderTargetView(backBuffer.Get(), nullptr, impl_->renderTargetView.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateRenderTargetView failed: " + HrToString(hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC backBufferDesc{};
    backBuffer->GetDesc(&backBufferDesc);

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = backBufferDesc.Width;
    depthDesc.Height = backBufferDesc.Height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = impl_->device->CreateTexture2D(&depthDesc, nullptr, impl_->depthTexture.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create depth buffer failed: " + HrToString(hr);
        return false;
    }

    hr = impl_->device->CreateDepthStencilView(impl_->depthTexture.Get(), nullptr, impl_->depthStencilView.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateDepthStencilView failed: " + HrToString(hr);
        return false;
    }

    return true;
}

bool Dx11PreviewRenderer::CreateShaders(std::string& error) {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errBlob;

    HRESULT hr = D3DCompile(
        kVertexShaderSource,
        std::strlen(kVertexShaderSource),
        "RipperForgeVS",
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        0,
        0,
        vsBlob.GetAddressOf(),
        errBlob.GetAddressOf());
    if (FAILED(hr)) {
        error = "Vertex shader compile failed.";
        if (errBlob != nullptr) {
            error += " ";
            error.append(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize());
        }
        return false;
    }

    errBlob.Reset();
    hr = D3DCompile(
        kPixelShaderSource,
        std::strlen(kPixelShaderSource),
        "RipperForgePS",
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        0,
        0,
        psBlob.GetAddressOf(),
        errBlob.GetAddressOf());
    if (FAILED(hr)) {
        error = "Pixel shader compile failed.";
        if (errBlob != nullptr) {
            error += " ";
            error.append(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize());
        }
        return false;
    }

    hr = impl_->device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, impl_->vertexShader.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateVertexShader failed: " + HrToString(hr);
        return false;
    }

    hr = impl_->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, impl_->pixelShader.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreatePixelShader failed: " + HrToString(hr);
        return false;
    }

    const std::array<D3D11_INPUT_ELEMENT_DESC, 3> inputElements = {
        D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SceneVertex, px), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SceneVertex, nx), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SceneVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = impl_->device->CreateInputLayout(
        inputElements.data(),
        static_cast<UINT>(inputElements.size()),
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        impl_->inputLayout.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateInputLayout failed: " + HrToString(hr);
        return false;
    }

    return true;
}

bool Dx11PreviewRenderer::CreatePipelineState(std::string& error) {
    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(SceneCBuffer);
    constantDesc.Usage = D3D11_USAGE_DEFAULT;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    HRESULT hr = impl_->device->CreateBuffer(&constantDesc, nullptr, impl_->constantBuffer.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create constant buffer failed: " + HrToString(hr);
        return false;
    }

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.DepthClipEnable = TRUE;
    hr = impl_->device->CreateRasterizerState(&rasterDesc, impl_->rasterizerState.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create rasterizer state failed: " + HrToString(hr);
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = impl_->device->CreateSamplerState(&samplerDesc, impl_->samplerState.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create sampler state failed: " + HrToString(hr);
        return false;
    }

    return true;
}

bool Dx11PreviewRenderer::CreateStaticGeometry(std::string& error) {
    const std::array<SceneVertex, 4> quadVertices = {
        SceneVertex{-1.0f, -1.0f, 0.0f, 0, 0, -1, 0, 1},
        SceneVertex{-1.0f, 1.0f, 0.0f, 0, 0, -1, 0, 0},
        SceneVertex{1.0f, 1.0f, 0.0f, 0, 0, -1, 1, 0},
        SceneVertex{1.0f, -1.0f, 0.0f, 0, 0, -1, 1, 1},
    };
    const std::array<uint32_t, 6> quadIndices = {0, 1, 2, 0, 2, 3};

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = static_cast<UINT>(sizeof(SceneVertex) * quadVertices.size());
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = quadVertices.data();

    HRESULT hr = impl_->device->CreateBuffer(&vbDesc, &vbData, impl_->quadVertexBuffer.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create quad vertex buffer failed: " + HrToString(hr);
        return false;
    }

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * quadIndices.size());
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = quadIndices.data();

    hr = impl_->device->CreateBuffer(&ibDesc, &ibData, impl_->quadIndexBuffer.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create quad index buffer failed: " + HrToString(hr);
        return false;
    }

    impl_->quadIndexCount = static_cast<uint32_t>(quadIndices.size());
    return true;
}

bool Dx11PreviewRenderer::UpdateTextureResource(const TextureData& texture, std::string& error) {
    if (impl_ == nullptr || impl_->device == nullptr) {
        error = "Renderer is not initialized.";
        return false;
    }

    if (texture.width == 0 || texture.height == 0 || texture.rgba8.empty()) {
        error = "Invalid texture payload.";
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = texture.width;
    textureDesc.Height = texture.height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = texture.rgba8.data();
    initialData.SysMemPitch = texture.width * 4;

    ComPtr<ID3D11Texture2D> textureResource;
    HRESULT hr = impl_->device->CreateTexture2D(&textureDesc, &initialData, textureResource.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateTexture2D failed: " + HrToString(hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = impl_->device->CreateShaderResourceView(textureResource.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateShaderResourceView failed: " + HrToString(hr);
        return false;
    }

    impl_->textureSrv = std::move(srv);
    return true;
}

bool Dx11PreviewRenderer::UpdateMeshBuffers(const MeshData& mesh, std::string& error) {
    if (impl_ == nullptr || impl_->device == nullptr) {
        error = "Renderer is not initialized.";
        return false;
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "Invalid mesh payload.";
        return false;
    }

    std::vector<SceneVertex> sceneVertices;
    sceneVertices.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
        SceneVertex sv{};
        sv.px = v.px;
        sv.py = v.py;
        sv.pz = v.pz;
        sv.nx = v.nx;
        sv.ny = v.ny;
        sv.nz = v.nz;
        sv.u = v.u;
        sv.v = v.v;
        sceneVertices.push_back(sv);
    }

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = static_cast<UINT>(sceneVertices.size() * sizeof(SceneVertex));
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = sceneVertices.data();

    ComPtr<ID3D11Buffer> vertexBuffer;
    HRESULT hr = impl_->device->CreateBuffer(&vbDesc, &vbData, vertexBuffer.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create model vertex buffer failed: " + HrToString(hr);
        return false;
    }

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = mesh.indices.data();

    ComPtr<ID3D11Buffer> indexBuffer;
    hr = impl_->device->CreateBuffer(&ibDesc, &ibData, indexBuffer.GetAddressOf());
    if (FAILED(hr)) {
        error = "Create model index buffer failed: " + HrToString(hr);
        return false;
    }

    impl_->modelVertexBuffer = std::move(vertexBuffer);
    impl_->modelIndexBuffer = std::move(indexBuffer);
    impl_->modelIndexCount = static_cast<uint32_t>(mesh.indices.size());
    return true;
}

} // namespace rf::core
