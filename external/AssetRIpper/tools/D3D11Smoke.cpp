#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace fs = std::filesystem;

struct Vertex {
    float pos[3];
    float norm[3];
    float uv[2];
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

int wmain() {
    auto logLine = [](const char* msg) {
        std::printf("%s\n", msg);
        std::ofstream out("tools\\smoke.log", std::ios::app);
        if (out.is_open()) {
            out << msg << "\n";
        }
    };

    logLine("Start");

    const wchar_t* kClass = L"AR_SmokeWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(kClass, L"AssetRipper Smoke", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        logLine("Failed to create window.");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    logLine("Waiting for injection");
    const DWORD waitStart = GetTickCount();
    const DWORD waitMs = 5000;
    while (GetTickCount() - waitStart < waitMs) {
        if (GetModuleHandleW(L"ripper_new6.dll")) break;
        Sleep(50);
    }
    Sleep(500);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               &fl, 1, D3D11_SDK_VERSION, &sd, &swap,
                                               &device, nullptr, &ctx);
    if (FAILED(hr)) {
        logLine("Hardware device creation failed; trying WARP.");
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           &fl, 1, D3D11_SDK_VERSION, &sd, &swap,
                                           &device, nullptr, &ctx);
    }
    if (FAILED(hr)) {
        logLine("D3D11CreateDeviceAndSwapChain failed.");
        return 1;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    if (backbuffer) backbuffer->Release();

    const char* hlsl =
        "struct VSIn { float3 pos : POSITION; float3 norm : NORMAL; float2 uv : TEXCOORD0; };"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "VSOut vs_main(VSIn v) { VSOut o; o.pos = float4(v.pos, 1); o.uv = v.uv; return o; }"
        "Texture2D tex0 : register(t0); SamplerState samp0 : register(s0);"
        "float4 ps_main(VSOut v) : SV_Target { return tex0.Sample(samp0, v.uv); }";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "vs_main", "vs_5_0", flags, 0, &vsBlob, &errors);
    if (FAILED(hr)) {
        logLine("Vertex shader compile failed.");
        if (errors) errors->Release();
        return 1;
    }
    if (errors) errors->Release();
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "ps_main", "ps_5_0", flags, 0, &psBlob, &errors);
    if (FAILED(hr)) {
        logLine("Pixel shader compile failed.");
        if (errors) errors->Release();
        return 1;
    }
    if (errors) errors->Release();

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs))) {
        logLine("CreateVertexShader failed.");
        return 1;
    }
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps))) {
        logLine("CreatePixelShader failed.");
        return 1;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout* inputLayout = nullptr;
    if (FAILED(device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout))) {
        logLine("CreateInputLayout failed.");
        return 1;
    }

    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();

    Vertex verts[3] = {
        { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  0.0f,  0.5f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.5f, 0.0f } },
        { {  0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },
    };
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(verts);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{ verts };
    ID3D11Buffer* vb = nullptr;
    if (FAILED(device->CreateBuffer(&vbDesc, &vbData, &vb))) {
        logLine("CreateBuffer(vb) failed.");
        return 1;
    }

    const uint32_t texPixels[4] = {
        0xFF0000FFu, 0xFF00FF00u,
        0xFFFF0000u, 0xFFFFFFFFu,
    };
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = 2;
    texDesc.Height = 2;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA texData{ texPixels, 2 * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&texDesc, &texData, &tex))) {
        logLine("CreateTexture2D failed.");
        return 1;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateShaderResourceView(tex, &srvDesc, &srv))) {
        logLine("CreateShaderResourceView failed.");
        return 1;
    }

    if (tex) tex->Release();

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* samp = nullptr;
    if (FAILED(device->CreateSamplerState(&sampDesc, &samp))) {
        logLine("CreateSamplerState failed.");
        return 1;
    }

    struct IndirectArgs {
        UINT VertexCountPerInstance;
        UINT InstanceCount;
        UINT StartVertex;
        UINT StartInstance;
    } args = { 3, 1, 0, 0 };

    D3D11_BUFFER_DESC argsDesc{};
    argsDesc.ByteWidth = sizeof(IndirectArgs);
    argsDesc.Usage = D3D11_USAGE_DEFAULT;
    argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    D3D11_SUBRESOURCE_DATA argsData{ &args };
    ID3D11Buffer* argsBuffer = nullptr;
    if (FAILED(device->CreateBuffer(&argsDesc, &argsData, &argsBuffer))) {
        logLine("CreateBuffer(args) failed.");
        return 1;
    }

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetInputLayout(inputLayout);
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &samp);

    D3D11_VIEWPORT vp{};
    vp.Width = 640.0f;
    vp.Height = 480.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    logLine("Ready for injection");
    Sleep(500);

    DWORD startTick = GetTickCount();
    const DWORD runMs = 1000;
    while (GetTickCount() - startTick < runMs) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                startTick = GetTickCount() - runMs - 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        float clear[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->DrawInstancedIndirect(argsBuffer, 0);
        ctx->Draw(3, 0);
        swap->Present(0, 0);
        Sleep(16);
    }

    if (argsBuffer) argsBuffer->Release();
    if (samp) samp->Release();
    if (srv) srv->Release();
    if (vb) vb->Release();
    if (inputLayout) inputLayout->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (rtv) rtv->Release();
    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (device) device->Release();

    DestroyWindow(hwnd);
    UnregisterClassW(kClass, wc.hInstance);

    logLine("Done");
    return 0;
}
