#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <detours.h>
#include <initguid.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <memory>
#include <limits>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "detours.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

// Basic definitions to keep the sample self-contained
namespace fs = std::filesystem;

static HMODULE g_hModule = nullptr;
static fs::path g_outputDir;
static std::atomic<bool> g_captureRequested{false};
static std::atomic<bool> g_captureEveryFrame{false};
static std::atomic<uint32_t> g_autoCaptureMaxPerWindow{60};
static std::atomic<uint32_t> g_autoCaptureWindowMs{1000};
static std::atomic<uint64_t> g_autoCaptureWindowStartMs{0};
static std::atomic<uint32_t> g_autoCaptureCount{0};
static std::atomic<bool> g_shutdown{false};
static std::atomic<uint64_t> g_meshCounter{0};
static std::atomic<uint64_t> g_texCounter{0};
static std::atomic<uint64_t> g_shaderCounter{0};
static std::atomic<bool> g_frameCaptureRequested{false};
static std::atomic<bool> g_frameCaptureActive{false};
static std::atomic<bool> g_frameCapturePendingStart{false};
static std::atomic<uint64_t> g_frameCounter{0};
static std::atomic<bool> g_gltfFlipZ{true};
static std::atomic<bool> g_gltfFlipWinding{true};
static std::atomic<bool> g_gltfFlipV{false};
static std::atomic<bool> g_gltfFlipNormalGreen{false};
static std::atomic<bool> g_gltfFlipTangentW{false};
static std::atomic<bool> g_gltfDedupMeshes{true};
static std::atomic<bool> g_gltfDedupTextures{true};
static std::atomic<bool> g_gltfDedupSamplers{true};

enum class CaptureType {
    Mesh,
    Texture,
    Shader,
    Frame
};

enum class ShaderStage {
    Unknown,
    Vertex,
    Pixel
};

struct InputElementInfo {
    std::string semantic;
    UINT semanticIndex{0};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    UINT slot{0};
    UINT offset{0};
    D3D11_INPUT_CLASSIFICATION inputClass{D3D11_INPUT_PER_VERTEX_DATA};
    UINT stepRate{0};
};

static constexpr UINT kMaxFrameVertexBuffers = 8;
static constexpr UINT kMaxFrameSrvs = 16;
static constexpr UINT kMaxFrameSamplers = 16;
static constexpr UINT kMaxFrameConstantBuffers = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;

struct VertexStreamCapture {
    UINT slot{0};
    UINT stride{0};
    UINT offset{0};
    std::vector<uint8_t> data;
};

struct InstanceStreamCapture {
    UINT slot{0};
    UINT stride{0};
    UINT startInstance{0};
    UINT instanceCount{0};
    std::vector<uint8_t> data;
};

struct BufferCapture {
    UINT slot{0};
    std::vector<uint8_t> data;
};

struct TextureCapture {
    std::vector<uint8_t> data;
    UINT width{0};
    UINT height{0};
    UINT rowPitch{0};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    bool isSrgb{false};
    uint64_t hash{0};
};

struct SamplerCapture {
    D3D11_SAMPLER_DESC desc{};
    uint64_t hash{0};
};

struct MatrixSource {
    bool valid{false};
    ShaderStage stage{ShaderStage::Unknown};
    UINT slot{0};
    UINT offsetBytes{0};
    bool columnMajor{true};
};

struct DrawCapture {
    bool indexed{false};
    D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
    std::vector<InputElementInfo> inputLayout;
    std::vector<VertexStreamCapture> vertexStreams;
    std::vector<uint32_t> indices;
    uint32_t vertexCount{0};
    uint32_t minIndex{0};
    uint32_t maxIndex{0};
    UINT instanceCount{1};
    UINT startInstance{0};
    std::vector<InstanceStreamCapture> instanceStreams;
    std::vector<BufferCapture> vsConstantBuffers;
    std::vector<BufferCapture> psConstantBuffers;
    std::array<int, kMaxFrameSrvs> srvTextures{};
    std::array<int, kMaxFrameSamplers> samplerBindings{};
    bool hasTransform{false};
    float transform[16]{};
    MatrixSource transformSource{};

    DrawCapture() {
        srvTextures.fill(-1);
        samplerBindings.fill(-1);
    }
};

struct FrameCapture {
    uint64_t frameIndex{0};
    std::vector<DrawCapture> draws;
    std::vector<TextureCapture> textures;
    std::vector<SamplerCapture> samplers;
    std::unordered_map<void*, size_t> texturePtrToIndex;
    std::unordered_map<uint64_t, size_t> textureHashToIndex;
    std::unordered_map<uint64_t, size_t> samplerHashToIndex;
};

struct CaptureItem {
    CaptureType type{};
    std::vector<uint8_t> vertexData;
    std::vector<uint32_t> indices;
    UINT stride{0};
    UINT vertexCount{0};
    UINT vertexSlot{0};
    std::vector<InputElementInfo> inputLayout;
    std::vector<uint8_t> textureData;
    UINT texWidth{0};
    UINT texHeight{0};
    UINT texRowPitch{0};
    DXGI_FORMAT texFormat{DXGI_FORMAT_UNKNOWN};
    UINT texMipLevels{1};
    UINT texArraySize{1};
    bool texIsCube{false};
    D3D11_RESOURCE_DIMENSION texDimension{D3D11_RESOURCE_DIMENSION_UNKNOWN};
    std::vector<uint8_t> shaderBytes;
    ShaderStage shaderStage{ShaderStage::Unknown};
    std::unique_ptr<FrameCapture> frame;
};

static std::mutex g_queueMutex;
static std::condition_variable g_queueCv;
static std::deque<CaptureItem> g_queue;
static std::thread g_writerThread;
static HANDLE g_hotkeyThreadHandle = nullptr;
static DWORD g_hotkeyThreadId = 0;
static std::thread g_watchThread;
static HWND g_debugWnd = nullptr;
static HWND g_debugEdit = nullptr;
static HWND g_overlayWnd = nullptr;
static std::wstring g_overlayLine1;
static std::wstring g_overlayLine2;
static std::mutex g_overlayMutex;
static std::atomic<bool> g_overlayEnabled{true};
enum class GraphicsApi { Unknown, DX9, DX11, DX12, Vulkan };
static std::atomic<GraphicsApi> g_api{GraphicsApi::Unknown};
static HANDLE g_debugThreadHandle = nullptr;
static HANDLE g_overlayThreadHandle = nullptr;
static HANDLE g_cleanupThreadHandle = nullptr;
static std::atomic<bool> g_cleanupRequested{false};
static std::mutex g_layoutMutex;
static std::unordered_map<void*, std::vector<InputElementInfo>> g_layouts;
static std::mutex g_frameMutex;
static FrameCapture g_frameCapture;
static size_t g_queueBytes = 0;
static constexpr size_t kMaxQueueBytes = 128 * 1024 * 1024;
static constexpr size_t kMaxQueueItems = 256;
static constexpr size_t kMaxConstantBufferCaptureBytes = 256 * 1024;
static constexpr size_t kMaxInstanceBufferCaptureBytes = 8 * 1024 * 1024;

enum class LogOnceId {
    QueueDropped,
    UnsupportedTopology,
    MissingInputLayout,
    MissingPosition,
    MapStagingVBFailed,
    MapStagingIBFailed,
    CreateStagingTextureFailed,
    MapStagingSRVFailed,
    SRVNotTexture2D,
    IndirectArgsFailed,
    IndirectInstancedSeen,
    IndirectIndexedSeen,
    UnsupportedTextureView,
    TypelessTextureFormat,
    UnsupportedTextureMSAA,
    UnsupportedIndexFormat,
    InvalidVertexRange,
    D3D9VertexDeclMissing,
    D3D9VertexDeclFormat,
    GltfFrameEmpty,
    GltfUnsupportedTextureFormat,
    GltfPngEncodeFailed,
    Count
};

static std::atomic<bool> g_logOnce[static_cast<size_t>(LogOnceId::Count)]{};

static constexpr uint32_t DDSD_CAPS = 0x1;
static constexpr uint32_t DDSD_HEIGHT = 0x2;
static constexpr uint32_t DDSD_WIDTH = 0x4;
static constexpr uint32_t DDSD_PITCH = 0x8;
static constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
static constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
static constexpr uint32_t DDSD_LINEARSIZE = 0x80000;
static constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;
static constexpr uint32_t DDSCAPS_COMPLEX = 0x8;
static constexpr uint32_t DDSCAPS_MIPMAP = 0x400000;
static constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200;
static constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEX = 0x400;
static constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEX = 0x800;
static constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEY = 0x1000;
static constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEY = 0x2000;
static constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEZ = 0x4000;
static constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x8000;
static constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
static constexpr uint32_t DDPF_FOURCC = 0x4;
static constexpr uint32_t DDPF_RGB = 0x40;
static constexpr UINT WM_AR_APPEND_LOG = WM_APP + 1;
static constexpr UINT WM_AR_STOP = WM_APP + 2;
static constexpr UINT WM_AR_OVERLAY = WM_APP + 3;

#ifndef WKPDID_D3D11ShaderBytecode
DEFINE_GUID(WKPDID_D3D11ShaderBytecode, 0x7695d254, 0x1606, 0x4e1e, 0xa7, 0xd0, 0x4e, 0x35, 0xf4, 0x26, 0x3d, 0x34);
#endif

// Forward declarations for hooks
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT (STDMETHODCALLTYPE* D3D9CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE* DrawIndexedPrimitive9_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present9_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

typedef HRESULT (WINAPI* D3D11CreateDevice_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI* D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (STDMETHODCALLTYPE* CreateInputLayout_t)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*,
                                                        UINT, const void*, SIZE_T, ID3D11InputLayout**);
typedef HRESULT (STDMETHODCALLTYPE* CreateVertexShader_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
typedef HRESULT (STDMETHODCALLTYPE* CreatePixelShader_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

typedef void (STDMETHODCALLTYPE* DrawIndexed11_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* Draw11_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* DrawInstanced11_t)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* DrawIndexedInstanced11_t)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE* DrawInstancedIndirect11_t)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void (STDMETHODCALLTYPE* DrawIndexedInstancedIndirect11_t)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PresentDXGI_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present1DXGI_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffersDXGI_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static Direct3DCreate9_t g_origDirect3DCreate9 = nullptr;
static D3D9CreateDevice_t g_origD3D9CreateDevice = nullptr;
static DrawIndexedPrimitive9_t g_origDrawIndexedPrimitive9 = nullptr;
static Present9_t g_origD3D9Present = nullptr;
static D3D11CreateDevice_t g_origD3D11CreateDevice = nullptr;
static D3D11CreateDeviceAndSwapChain_t g_origD3D11CreateDeviceAndSwapChain = nullptr;
static CreateInputLayout_t g_origCreateInputLayout = nullptr;
static CreateVertexShader_t g_origCreateVertexShader = nullptr;
static CreatePixelShader_t g_origCreatePixelShader = nullptr;
static DrawIndexed11_t g_origDrawIndexed11 = nullptr;
static Draw11_t g_origDraw11 = nullptr;
static DrawInstanced11_t g_origDrawInstanced11 = nullptr;
static DrawIndexedInstanced11_t g_origDrawIndexedInstanced11 = nullptr;
static DrawInstancedIndirect11_t g_origDrawInstancedIndirect11 = nullptr;
static DrawIndexedInstancedIndirect11_t g_origDrawIndexedInstancedIndirect11 = nullptr;
static PresentDXGI_t g_origDXGIPresent = nullptr;
static Present1DXGI_t g_origDXGIPresent1 = nullptr;
static ResizeBuffersDXGI_t g_origDXGIResizeBuffers = nullptr;

static bool g_d3d9Hooked = false;
static bool g_d3d11Hooked = false;
static bool g_d3d11DeviceHooked = false;
static bool g_d3d9CreateHooked = false;
static bool g_d3d11CreateHooked = false;
static bool g_d3d9FactoryHooked = false;
static bool g_d3d9PresentHooked = false;
static bool g_d3d11PresentHooked = false;
static bool g_d3d11Present1Hooked = false;

// Forward declarations
static HRESULT STDMETHODCALLTYPE HookedD3D9CreateDevice(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive9(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static HRESULT WINAPI HookedD3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static HRESULT WINAPI HookedD3D11CreateDeviceAndSwapChain(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static HRESULT STDMETHODCALLTYPE HookedCreateInputLayout(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static void STDMETHODCALLTYPE HookedDrawIndexed11(ID3D11DeviceContext*, UINT, UINT, INT);
static void STDMETHODCALLTYPE HookedDraw11(ID3D11DeviceContext*, UINT, UINT);
static void STDMETHODCALLTYPE HookedDrawInstanced11(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
static void STDMETHODCALLTYPE HookedDrawIndexedInstanced11(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
static void STDMETHODCALLTYPE HookedDrawInstancedIndirect11(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
static void STDMETHODCALLTYPE HookedDrawIndexedInstancedIndirect11(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
static HRESULT STDMETHODCALLTYPE HookedPresent9(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static HRESULT STDMETHODCALLTYPE HookedPresentDXGI(IDXGISwapChain*, UINT, UINT);
static HRESULT STDMETHODCALLTYPE HookedPresent1DXGI(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static HRESULT STDMETHODCALLTYPE HookedResizeBuffersDXGI(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static void TryHookD3D9();
static void TryHookD3D11();
static void BootstrapPresentHooks();
static void AttachD3D9DeviceHooks(IDirect3DDevice9* device);
static void AttachD3D11ContextHooks(ID3D11Device* device, ID3D11DeviceContext* ctx);
static void AttachD3D11DeviceHooks(ID3D11Device* device);
static void RemoveHooks();
static void SignalShutdownOnly();
static void RequestShutdown(const wchar_t* reason);
static void FinalizeFrameCapture(const wchar_t* reason);
static void LogOnce(LogOnceId id, const std::wstring& msg);
static void Enqueue(CaptureItem&& item);

// Utility RAII release helper
template <typename T>
struct ComPtrGuard {
    T* ptr{nullptr};
    ComPtrGuard() = default;
    explicit ComPtrGuard(T* p) : ptr(p) {}
    ~ComPtrGuard() { if (ptr) ptr->Release(); }
    T* operator->() const { return ptr; }
    operator bool() const { return ptr != nullptr; }
};

static constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

static bool IsTypelessFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}

static bool IsSrgbFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBlockCompressed(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static size_t BytesPerBlock(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;
    default:
        return 0;
    }
}

static size_t BitsPerPixel(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 64;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return 32;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SINT:
        return 16;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 8;
    default:
        return 0;
    }
}

static size_t CalcRowPitch(DXGI_FORMAT fmt, UINT width) {
    if (IsBlockCompressed(fmt)) {
        const size_t blockBytes = BytesPerBlock(fmt);
        const size_t blocksWide = std::max<size_t>(1, (width + 3) / 4);
        return blocksWide * blockBytes;
    }
    const size_t bpp = BitsPerPixel(fmt);
    return (width * bpp + 7) / 8;
}

static size_t CalcRowCount(DXGI_FORMAT fmt, UINT height) {
    if (IsBlockCompressed(fmt)) {
        return std::max<size_t>(1, (height + 3) / 4);
    }
    return height;
}

static size_t CalcSubresourceSize(DXGI_FORMAT fmt, UINT width, UINT height) {
    return CalcRowPitch(fmt, width) * CalcRowCount(fmt, height);
}

static UINT FormatSizeBytes(DXGI_FORMAT fmt) {
    const size_t bpp = BitsPerPixel(fmt);
    return static_cast<UINT>((bpp + 7) / 8);
}

static uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 1469598103934665603ull) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t HashCombine(uint64_t seed, uint64_t value) {
    return HashBytes(&value, sizeof(value), seed);
}

static uint64_t HashString(const std::string& value, uint64_t seed = 1469598103934665603ull) {
    return HashBytes(value.data(), value.size(), seed);
}

static float HalfToFloat(uint16_t h) {
    const uint16_t sign = (h >> 15) & 0x1;
    const uint16_t exp = (h >> 10) & 0x1f;
    const uint16_t mant = h & 0x3ff;
    if (exp == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mant), -24);
    }
    if (exp == 31) {
        return sign ? -INFINITY : INFINITY;
    }
    return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mant | 0x400), exp - 25);
}

static bool DecodeDXGIFormat(DXGI_FORMAT fmt, const uint8_t* src, float out[4], UINT& components) {
    components = 0;
    switch (fmt) {
    case DXGI_FORMAT_R32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
        components = 1;
        return true;
    }
    case DXGI_FORMAT_R32G32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = 0.0f; out[3] = 1.0f;
        components = 2;
        return true;
    }
    case DXGI_FORMAT_R32G32B32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = 1.0f;
        components = 3;
        return true;
    }
    case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = f[3];
        components = 4;
        return true;
    }
    case DXGI_FORMAT_R16_FLOAT: {
        const uint16_t* v = reinterpret_cast<const uint16_t*>(src);
        out[0] = HalfToFloat(v[0]); out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
        components = 1;
        return true;
    }
    case DXGI_FORMAT_R16G16_FLOAT: {
        const uint16_t* v = reinterpret_cast<const uint16_t*>(src);
        out[0] = HalfToFloat(v[0]); out[1] = HalfToFloat(v[1]); out[2] = 0.0f; out[3] = 1.0f;
        components = 2;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const uint16_t* v = reinterpret_cast<const uint16_t*>(src);
        out[0] = HalfToFloat(v[0]); out[1] = HalfToFloat(v[1]); out[2] = HalfToFloat(v[2]); out[3] = HalfToFloat(v[3]);
        components = 4;
        return true;
    }
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: {
        const uint8_t* v = reinterpret_cast<const uint8_t*>(src);
        out[0] = v[0] / 255.0f;
        out[1] = v[1] / 255.0f;
        out[2] = v[2] / 255.0f;
        out[3] = v[3] / 255.0f;
        components = 4;
        return true;
    }
    case DXGI_FORMAT_R8G8B8A8_SNORM: {
        const int8_t* v = reinterpret_cast<const int8_t*>(src);
        out[0] = std::max(-1.0f, v[0] / 127.0f);
        out[1] = std::max(-1.0f, v[1] / 127.0f);
        out[2] = std::max(-1.0f, v[2] / 127.0f);
        out[3] = std::max(-1.0f, v[3] / 127.0f);
        components = 4;
        return true;
    }
    case DXGI_FORMAT_R16G16_SNORM: {
        const int16_t* v = reinterpret_cast<const int16_t*>(src);
        out[0] = std::max(-1.0f, v[0] / 32767.0f);
        out[1] = std::max(-1.0f, v[1] / 32767.0f);
        out[2] = 0.0f; out[3] = 1.0f;
        components = 2;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_SNORM: {
        const int16_t* v = reinterpret_cast<const int16_t*>(src);
        out[0] = std::max(-1.0f, v[0] / 32767.0f);
        out[1] = std::max(-1.0f, v[1] / 32767.0f);
        out[2] = std::max(-1.0f, v[2] / 32767.0f);
        out[3] = std::max(-1.0f, v[3] / 32767.0f);
        components = 4;
        return true;
    }
    default:
        return false;
    }
}

static DXGI_FORMAT D3D9FormatToDXGI(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_X8R8G8B8:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case D3DFMT_A8B8G8R8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_DXT1:
        return DXGI_FORMAT_BC1_UNORM;
    case D3DFMT_DXT3:
        return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT5:
        return DXGI_FORMAT_BC3_UNORM;
    case D3DFMT_A16B16G16R16F:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case D3DFMT_A32B32G32R32F:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DFMT_R32F:
        return DXGI_FORMAT_R32_FLOAT;
    case D3DFMT_A8:
        return DXGI_FORMAT_A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

static bool D3D9DeclTypeToDXGI(BYTE type, DXGI_FORMAT& outFmt) {
    switch (type) {
    case D3DDECLTYPE_FLOAT1:
        outFmt = DXGI_FORMAT_R32_FLOAT; return true;
    case D3DDECLTYPE_FLOAT2:
        outFmt = DXGI_FORMAT_R32G32_FLOAT; return true;
    case D3DDECLTYPE_FLOAT3:
        outFmt = DXGI_FORMAT_R32G32B32_FLOAT; return true;
    case D3DDECLTYPE_FLOAT4:
        outFmt = DXGI_FORMAT_R32G32B32A32_FLOAT; return true;
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4N:
        outFmt = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    case D3DDECLTYPE_SHORT2N:
        outFmt = DXGI_FORMAT_R16G16_SNORM; return true;
    case D3DDECLTYPE_SHORT4N:
        outFmt = DXGI_FORMAT_R16G16B16A16_SNORM; return true;
    case D3DDECLTYPE_FLOAT16_2:
        outFmt = DXGI_FORMAT_R16G16_FLOAT; return true;
    case D3DDECLTYPE_FLOAT16_4:
        outFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; return true;
    default:
        return false;
    }
}

static std::string D3D9UsageToSemantic(BYTE usage) {
    switch (usage) {
    case D3DDECLUSAGE_POSITION: return "POSITION";
    case D3DDECLUSAGE_NORMAL: return "NORMAL";
    case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
    default: return "";
    }
}

static bool ReadBufferRange(ID3D11DeviceContext* ctx, ID3D11Device* device, ID3D11Buffer* buffer,
                            size_t offsetBytes, size_t sizeBytes, std::vector<uint8_t>& out) {
    if (!ctx || !device || !buffer || sizeBytes == 0) return false;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (offsetBytes + sizeBytes > desc.ByteWidth) return false;

    ID3D11Buffer* readBuffer = buffer;
    ComPtrGuard<ID3D11Buffer> staging;
    if (desc.Usage != D3D11_USAGE_STAGING || !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) {
        D3D11_BUFFER_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.StructureByteStride = 0;
        if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, &staging.ptr))) return false;
        ctx->CopyResource(staging.ptr, buffer);
        readBuffer = staging.ptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(readBuffer, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    out.resize(sizeBytes);
    memcpy(out.data(), reinterpret_cast<const uint8_t*>(mapped.pData) + offsetBytes, sizeBytes);
    ctx->Unmap(readBuffer, 0);
    return true;
}

static bool ReadVertexStreamRange(ID3D11DeviceContext* ctx, ID3D11Device* device, ID3D11Buffer* vb,
                                  UINT stride, UINT offsetBytes, uint32_t minIndex, uint32_t maxIndex,
                                  std::vector<uint8_t>& out) {
    if (!ctx || !device || !vb || stride == 0) return false;
    if (minIndex > maxIndex) return false;
    D3D11_BUFFER_DESC vbDesc{};
    vb->GetDesc(&vbDesc);
    if (vbDesc.ByteWidth == 0) return false;

    const uint32_t vertexCount = maxIndex - minIndex + 1;
    const size_t startByte = static_cast<size_t>(offsetBytes) + static_cast<size_t>(minIndex) * stride;
    const size_t byteCount = static_cast<size_t>(vertexCount) * stride;
    if (startByte + byteCount > vbDesc.ByteWidth) {
        std::wstringstream ss;
        ss << L"D3D11 vertex range exceeds buffer (offset=" << offsetBytes
           << L", stride=" << stride
           << L", minIndex=" << minIndex
           << L", maxIndex=" << maxIndex
           << L", byteWidth=" << vbDesc.ByteWidth << L"); skipping capture.";
        LogOnce(LogOnceId::InvalidVertexRange, ss.str());
        return false;
    }
    return ReadBufferRange(ctx, device, vb, startByte, byteCount, out);
}

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rMask;
    uint32_t gMask;
    uint32_t bMask;
    uint32_t aMask;
};

struct DDS_HEADER {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DDS_HEADER_DXT10 {
    DXGI_FORMAT dxgiFormat;
    D3D11_RESOURCE_DIMENSION resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

static std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) return {};
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                                         nullptr, 0);
    std::wstring out(sizeNeeded, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                        out.data(), sizeNeeded);
    return out;
}

static void AppendDebugLine(const std::wstring& line) {
    if (!g_debugWnd) return;
    auto* payload = new std::wstring(line);
    PostMessageW(g_debugWnd, WM_AR_APPEND_LOG, 0, reinterpret_cast<LPARAM>(payload));
}

static void UpdateOverlay(const std::wstring& line1, const std::wstring& line2 = L"") {
    if (!g_overlayEnabled.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        g_overlayLine1 = line1;
        g_overlayLine2 = line2;
    }
    if (g_overlayWnd) PostMessageW(g_overlayWnd, WM_AR_OVERLAY, 0, 0);
}

static fs::path GetConfigPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    DWORD pid = GetCurrentProcessId();
    std::wstringstream ss;
    ss << L"asset_ripper_" << pid << L".cfg";
    return fs::path(tempPath) / ss.str();
}

static void Log(const std::wstring& msg) {
    fs::path logPath = g_outputDir.empty() ? GetConfigPath() : g_outputDir / L"ripper.log";
    // Basic size cap to prevent runaway logs (e.g., 5 MB)
    if (fs::exists(logPath) && fs::file_size(logPath) > 5 * 1024 * 1024) {
        fs::remove(logPath);
    }
    std::wofstream log(logPath, std::ios::app);
    if (!log.is_open()) return;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::wstring formatted = L"[" + std::to_wstring(static_cast<long long>(now)) + L"] " + msg + L"\n";
    log << formatted;
    OutputDebugStringW((L"[AssetRipper] " + msg + L"\n").c_str());
    AppendDebugLine(formatted);
    if (g_overlayEnabled.load()) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        g_overlayLine1 = msg;
        g_overlayLine2.clear();
        if (g_overlayWnd) PostMessageW(g_overlayWnd, WM_AR_OVERLAY, 0, 0);
    }
}

static void LogOnce(LogOnceId id, const std::wstring& msg) {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(LogOnceId::Count)) return;
    bool expected = false;
    if (g_logOnce[idx].compare_exchange_strong(expected, true)) {
        Log(msg);
    }
}

static uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

static void ResetAutoCaptureThrottle() {
    g_autoCaptureWindowStartMs.store(0);
    g_autoCaptureCount.store(0);
}

static void SetAutoCaptureEnabled(bool enabled) {
    g_captureEveryFrame.store(enabled);
    if (enabled) ResetAutoCaptureThrottle();
}

static bool AllowAutoCapture() {
    const uint32_t maxPerWindow = g_autoCaptureMaxPerWindow.load();
    if (maxPerWindow == 0) return true;

    uint32_t windowMs = g_autoCaptureWindowMs.load();
    if (windowMs == 0) windowMs = 1000;

    const uint64_t now = NowMs();
    uint64_t start = g_autoCaptureWindowStartMs.load();
    if (start == 0 || now - start >= windowMs) {
        if (g_autoCaptureWindowStartMs.compare_exchange_strong(start, now)) {
            g_autoCaptureCount.store(0);
        }
    }

    const uint32_t count = g_autoCaptureCount.fetch_add(1) + 1;
    return count <= maxPerWindow;
}

static void RequestFrameCapture(const wchar_t* reason) {
    if (g_frameCaptureActive.load()) {
        FinalizeFrameCapture(L"Frame capture finalized by user.");
        return;
    }
    if (g_frameCapturePendingStart.load()) {
        g_frameCapturePendingStart.store(false);
        g_frameCaptureRequested.store(false);
        Log(L"Frame capture request canceled.");
        UpdateOverlay(L"Frame capture canceled", L"");
        return;
    }
    g_frameCaptureRequested.store(true);
    g_frameCapturePendingStart.store(true);
    if (reason) Log(reason);
    UpdateOverlay(L"Frame capture requested", L"F9");
}

static void StartFrameCaptureIfNeeded() {
    bool expected = true;
    if (!g_frameCapturePendingStart.compare_exchange_strong(expected, false)) return;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    g_frameCapture = FrameCapture{};
    g_frameCapture.frameIndex = ++g_frameCounter;
    g_frameCaptureActive.store(true);
    g_frameCaptureRequested.store(false);
    UpdateOverlay(L"Frame capture started", L"");
}

static void FinalizeFrameCapture(const wchar_t* reason) {
    if (!g_frameCaptureActive.load()) return;
    g_frameCaptureActive.store(false);
    FrameCapture frame;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        frame = std::move(g_frameCapture);
        g_frameCapture = FrameCapture{};
    }
    if (frame.draws.empty()) {
        LogOnce(LogOnceId::GltfFrameEmpty, L"Frame capture had no draws; skipping glTF export.");
        return;
    }
    CaptureItem item;
    item.type = CaptureType::Frame;
    item.frame = std::make_unique<FrameCapture>(std::move(frame));
    Enqueue(std::move(item));
    if (reason) Log(reason);
    UpdateOverlay(L"Frame capture queued", L"glTF export");
}

static std::wstring ApiToString(GraphicsApi api) {
    switch (api) {
    case GraphicsApi::DX9: return L"DX9";
    case GraphicsApi::DX11: return L"DX11";
    case GraphicsApi::DX12: return L"DX12";
    case GraphicsApi::Vulkan: return L"Vulkan";
    default: return L"Unknown";
    }
}

static void LogSessionStart() {
    wchar_t exePath[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t cwd[MAX_PATH] = L"";
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const DWORD pid = GetCurrentProcessId();
    const std::wstring cmd = GetCommandLineW();

    Log(L"=== AssetRipper session start ===");
    Log(L"PID=" + std::to_wstring(static_cast<unsigned long long>(pid)));
    Log(L"Exe=" + std::wstring(exePath));
    Log(L"CWD=" + std::wstring(cwd));
    Log(L"Cmd=" + cmd);
    Log(L"API detected=" + ApiToString(g_api.load()));

    std::wstringstream mods;
    mods << L"Modules: d3d9=" << (GetModuleHandleW(L"d3d9.dll") ? L"1" : L"0")
         << L", d3d11=" << (GetModuleHandleW(L"d3d11.dll") ? L"1" : L"0")
         << L", dxgi=" << (GetModuleHandleW(L"dxgi.dll") ? L"1" : L"0")
         << L", d3d12=" << (GetModuleHandleW(L"d3d12.dll") ? L"1" : L"0")
         << L", vulkan=" << (GetModuleHandleW(L"vulkan-1.dll") ? L"1" : L"0")
         << L", unity=" << (GetModuleHandleW(L"UnityPlayer.dll") ? L"1" : L"0");
    Log(mods.str());

    std::wstringstream cfg;
    cfg << L"Auto-capture=" << (g_captureEveryFrame.load() ? L"1" : L"0")
        << L", throttle=" << g_autoCaptureMaxPerWindow.load()
        << L" draws/" << g_autoCaptureWindowMs.load() << L"ms";
    Log(cfg.str());

    std::wstringstream gltf;
    gltf << L"glTF: flipZ=" << (g_gltfFlipZ.load() ? L"1" : L"0")
         << L", flipWinding=" << (g_gltfFlipWinding.load() ? L"1" : L"0")
         << L", flipV=" << (g_gltfFlipV.load() ? L"1" : L"0")
         << L", flipNormalGreen=" << (g_gltfFlipNormalGreen.load() ? L"1" : L"0")
         << L", flipTangentW=" << (g_gltfFlipTangentW.load() ? L"1" : L"0")
         << L", dedupMeshes=" << (g_gltfDedupMeshes.load() ? L"1" : L"0")
         << L", dedupTextures=" << (g_gltfDedupTextures.load() ? L"1" : L"0")
         << L", dedupSamplers=" << (g_gltfDedupSamplers.load() ? L"1" : L"0");
    Log(gltf.str());
}

static void EnsureOutputDir() {
    if (g_outputDir.empty()) {
        if (g_hModule) {
            wchar_t modulePath[MAX_PATH];
            GetModuleFileNameW(g_hModule, modulePath, MAX_PATH);
            fs::path base(modulePath);
            g_outputDir = base.parent_path() / "captures";
        }
    }
    if (!g_outputDir.empty()) {
        std::error_code ec;
        fs::create_directories(g_outputDir, ec);
    }
}

static size_t EstimateItemBytes(const CaptureItem& item) {
    size_t total = 0;
    switch (item.type) {
    case CaptureType::Mesh:
        total += item.vertexData.size();
        total += item.indices.size() * sizeof(uint32_t);
        break;
    case CaptureType::Texture:
        total += item.textureData.size();
        break;
    case CaptureType::Shader:
        total += item.shaderBytes.size();
        break;
    case CaptureType::Frame:
        if (item.frame) {
            for (const auto& tex : item.frame->textures) {
                total += tex.data.size();
            }
            for (const auto& draw : item.frame->draws) {
                total += draw.indices.size() * sizeof(uint32_t);
                for (const auto& stream : draw.vertexStreams) {
                    total += stream.data.size();
                }
                for (const auto& inst : draw.instanceStreams) {
                    total += inst.data.size();
                }
                for (const auto& cb : draw.vsConstantBuffers) {
                    total += cb.data.size();
                }
                for (const auto& cb : draw.psConstantBuffers) {
                    total += cb.data.size();
                }
            }
        }
        break;
    }
    return total;
}

static void Enqueue(CaptureItem&& item) {
    const size_t itemBytes = EstimateItemBytes(item);
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (item.type == CaptureType::Frame) {
            g_queue.clear();
            g_queueBytes = 0;
            g_queue.emplace_back(std::move(item));
            g_queueBytes = itemBytes;
        } else {
            g_queue.emplace_back(std::move(item));
            g_queueBytes += itemBytes;
            while ((g_queueBytes > kMaxQueueBytes || g_queue.size() > kMaxQueueItems) && !g_queue.empty()) {
                g_queueBytes -= EstimateItemBytes(g_queue.front());
                g_queue.pop_front();
                dropped = true;
            }
        }
    }
    if (dropped) {
        LogOnce(LogOnceId::QueueDropped, L"Capture queue overflow; dropping oldest items.");
    }
    g_queueCv.notify_one();
}

static bool SemanticEquals(const std::string& semantic, const char* name) {
    if (!name) return false;
    if (semantic.size() != std::strlen(name)) return false;
    for (size_t i = 0; i < semantic.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(semantic[i])) != std::toupper(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

static const InputElementInfo* FindElement(const std::vector<InputElementInfo>& elements,
                                           const char* semantic, UINT semanticIndex, UINT slot) {
    for (const auto& elem : elements) {
        if (elem.slot != slot) continue;
        if (elem.inputClass != D3D11_INPUT_PER_VERTEX_DATA) continue;
        if (elem.semanticIndex == semanticIndex && SemanticEquals(elem.semantic, semantic)) {
            return &elem;
        }
    }
    return nullptr;
}

static const InputElementInfo* FindElementAnySlot(const std::vector<InputElementInfo>& elements,
                                                  const char* semantic, UINT semanticIndex,
                                                  D3D11_INPUT_CLASSIFICATION inputClass = D3D11_INPUT_PER_VERTEX_DATA) {
    for (const auto& elem : elements) {
        if (elem.inputClass != inputClass) continue;
        if (elem.semanticIndex == semanticIndex && SemanticEquals(elem.semantic, semantic)) {
            return &elem;
        }
    }
    return nullptr;
}

static const VertexStreamCapture* FindVertexStream(const std::vector<VertexStreamCapture>& streams, UINT slot) {
    for (const auto& stream : streams) {
        if (stream.slot == slot) return &stream;
    }
    return nullptr;
}

static uint64_t HashInputLayout(const std::vector<InputElementInfo>& elements) {
    uint64_t hash = 1469598103934665603ull;
    for (const auto& elem : elements) {
        hash = HashString(elem.semantic, hash);
        hash = HashCombine(hash, elem.semanticIndex);
        hash = HashCombine(hash, static_cast<uint64_t>(elem.format));
        hash = HashCombine(hash, elem.slot);
        hash = HashCombine(hash, elem.offset);
        hash = HashCombine(hash, static_cast<uint64_t>(elem.inputClass));
        hash = HashCombine(hash, elem.stepRate);
    }
    return hash;
}

static void SaveMesh(const CaptureItem& item) {
    EnsureOutputDir();
    const uint64_t idx = ++g_meshCounter;
    std::wstringstream ss;
    ss << L"model_" << std::setfill(L'0') << std::setw(3) << idx << L".obj";
    fs::path path = g_outputDir / ss.str();

    if (item.inputLayout.empty()) {
        LogOnce(LogOnceId::MissingInputLayout, L"No input layout metadata for mesh capture; skipping OBJ.");
        return;
    }

    const InputElementInfo* posElem = FindElement(item.inputLayout, "POSITION", 0, item.vertexSlot);
    if (!posElem) {
        LogOnce(LogOnceId::MissingPosition, L"No POSITION semantic found for mesh capture; skipping OBJ.");
        return;
    }
    const InputElementInfo* normElem = FindElement(item.inputLayout, "NORMAL", 0, item.vertexSlot);
    const InputElementInfo* uvElem = FindElement(item.inputLayout, "TEXCOORD", 0, item.vertexSlot);

    std::ofstream out(path);
    if (!out.is_open()) { Log(L"Failed to open mesh file " + path.wstring()); return; }

    const size_t vertexCount = item.vertexCount;
    std::vector<std::array<float, 4>> positions(vertexCount);
    std::vector<std::array<float, 4>> normals;
    std::vector<std::array<float, 4>> uvs;
    if (normElem) normals.resize(vertexCount);
    if (uvElem) uvs.resize(vertexCount);

    for (size_t i = 0; i < vertexCount; ++i) {
        const uint8_t* base = item.vertexData.data() + i * item.stride;
        float decoded[4]{};
        UINT components = 0;
        if (posElem->offset + FormatSizeBytes(posElem->format) > item.stride ||
            !DecodeDXGIFormat(posElem->format, base + posElem->offset, decoded, components) || components < 3) {
            LogOnce(LogOnceId::InvalidVertexRange, L"Failed to decode POSITION from vertex buffer; skipping OBJ.");
            return;
        }
        positions[i] = {decoded[0], decoded[1], decoded[2], decoded[3]};

        if (normElem) {
            if (normElem->offset + FormatSizeBytes(normElem->format) <= item.stride &&
                DecodeDXGIFormat(normElem->format, base + normElem->offset, decoded, components) && components >= 3) {
                normals[i] = {decoded[0], decoded[1], decoded[2], decoded[3]};
            } else {
                normals[i] = {0.0f, 0.0f, 0.0f, 0.0f};
            }
        }

        if (uvElem) {
            if (uvElem->offset + FormatSizeBytes(uvElem->format) <= item.stride &&
                DecodeDXGIFormat(uvElem->format, base + uvElem->offset, decoded, components) && components >= 2) {
                uvs[i] = {decoded[0], decoded[1], decoded[2], decoded[3]};
            } else {
                uvs[i] = {0.0f, 0.0f, 0.0f, 0.0f};
            }
        }
    }

    for (size_t i = 0; i < vertexCount; ++i) {
        out << "v " << positions[i][0] << " " << positions[i][1] << " " << positions[i][2] << "\n";
    }
    if (uvElem) {
        for (size_t i = 0; i < vertexCount; ++i) {
            out << "vt " << uvs[i][0] << " " << uvs[i][1] << "\n";
        }
    }
    if (normElem) {
        for (size_t i = 0; i < vertexCount; ++i) {
            out << "vn " << normals[i][0] << " " << normals[i][1] << " " << normals[i][2] << "\n";
        }
    }

    const bool hasUv = uvElem != nullptr;
    const bool hasNormal = normElem != nullptr;
    const auto& indices = item.indices;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i] + 1;
        const uint32_t b = indices[i + 1] + 1;
        const uint32_t c = indices[i + 2] + 1;
        if (hasUv && hasNormal) {
            out << "f " << a << "/" << a << "/" << a << " "
                << b << "/" << b << "/" << b << " "
                << c << "/" << c << "/" << c << "\n";
        } else if (hasUv) {
            out << "f " << a << "/" << a << " " << b << "/" << b << " " << c << "/" << c << "\n";
        } else if (hasNormal) {
            out << "f " << a << "//" << a << " " << b << "//" << b << " " << c << "//" << c << "\n";
        } else {
            out << "f " << a << " " << b << " " << c << "\n";
        }
    }
    UpdateOverlay(L"Saved mesh OBJ", ss.str());
}

static void SaveTexture(const CaptureItem& item) {
    EnsureOutputDir();
    const uint64_t idx = ++g_texCounter;
    std::wstringstream ss;
    ss << L"tex_" << std::setfill(L'0') << std::setw(3) << idx << L".dds";
    fs::path path = g_outputDir / ss.str();
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) { Log(L"Failed to open texture file " + path.wstring()); return; }

    if (item.texFormat == DXGI_FORMAT_UNKNOWN || IsTypelessFormat(item.texFormat)) {
        LogOnce(LogOnceId::TypelessTextureFormat, L"Skipping texture save due to unknown/typeless format.");
        return;
    }
    if (item.textureData.empty()) {
        return;
    }

    const bool compressed = IsBlockCompressed(item.texFormat);
    const size_t rowPitch = CalcRowPitch(item.texFormat, item.texWidth);
    const size_t linearSize = CalcSubresourceSize(item.texFormat, item.texWidth, item.texHeight);

    DDS_HEADER header{};
    header.size = sizeof(DDS_HEADER);
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    header.flags |= compressed ? DDSD_LINEARSIZE : DDSD_PITCH;
    if (item.texMipLevels > 1) header.flags |= DDSD_MIPMAPCOUNT;
    header.height = item.texHeight;
    header.width = item.texWidth;
    header.pitchOrLinearSize = static_cast<uint32_t>(compressed ? linearSize : rowPitch);
    header.depth = 0;
    header.mipMapCount = item.texMipLevels;
    header.ddspf.size = sizeof(DDS_PIXELFORMAT);
    header.ddspf.flags = DDPF_FOURCC;
    header.ddspf.fourCC = MakeFourCC('D', 'X', '1', '0');
    header.caps = DDSCAPS_TEXTURE;
    if (item.texMipLevels > 1) header.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    if (item.texArraySize > 1 || item.texIsCube) header.caps |= DDSCAPS_COMPLEX;
    if (item.texIsCube) {
        header.caps2 = DDSCAPS2_CUBEMAP |
                       DDSCAPS2_CUBEMAP_POSITIVEX | DDSCAPS2_CUBEMAP_NEGATIVEX |
                       DDSCAPS2_CUBEMAP_POSITIVEY | DDSCAPS2_CUBEMAP_NEGATIVEY |
                       DDSCAPS2_CUBEMAP_POSITIVEZ | DDSCAPS2_CUBEMAP_NEGATIVEZ;
    }
    DDS_HEADER_DXT10 dx10{};
    dx10.dxgiFormat = item.texFormat;
    dx10.resourceDimension = item.texDimension == D3D11_RESOURCE_DIMENSION_UNKNOWN
        ? D3D11_RESOURCE_DIMENSION_TEXTURE2D
        : item.texDimension;
    dx10.miscFlag = item.texIsCube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    uint32_t arraySize = item.texArraySize;
    if (item.texIsCube && arraySize >= 6) arraySize /= 6;
    dx10.arraySize = std::max(1u, arraySize);
    dx10.miscFlags2 = 0;

    uint32_t magic = 0x20534444; // "DDS "
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&dx10), sizeof(dx10));
    out.write(reinterpret_cast<const char*>(item.textureData.data()), static_cast<std::streamsize>(item.textureData.size()));
    UpdateOverlay(L"Saved texture DDS", ss.str());
}

static void SaveShader(const CaptureItem& item) {
    EnsureOutputDir();
    const uint64_t idx = ++g_shaderCounter;
    std::wstringstream ss;
    const wchar_t* stage = L"shader";
    if (item.shaderStage == ShaderStage::Vertex) stage = L"vs";
    else if (item.shaderStage == ShaderStage::Pixel) stage = L"ps";
    ss << stage << L"_" << std::setfill(L'0') << std::setw(3) << idx << L".dxbc";
    fs::path path = g_outputDir / ss.str();
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) { Log(L"Failed to open shader file " + path.wstring()); return; }
    if (!item.shaderBytes.empty()) {
        out.write(reinterpret_cast<const char*>(item.shaderBytes.data()),
                  static_cast<std::streamsize>(item.shaderBytes.size()));
    }
    UpdateOverlay(L"Saved shader", ss.str());
}

static uint32_t AppendToBin(std::vector<uint8_t>& bin, const void* data, size_t size, uint32_t alignment = 4) {
    const size_t padding = (alignment == 0) ? 0 : ((alignment - (bin.size() % alignment)) % alignment);
    bin.insert(bin.end(), padding, 0);
    const uint32_t offset = static_cast<uint32_t>(bin.size());
    if (data && size > 0) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        bin.insert(bin.end(), bytes, bytes + size);
    }
    return offset;
}

static bool ConvertTextureToRgba(const TextureCapture& tex, std::vector<uint8_t>& outRgba, bool flipGreen) {
    const bool isRgba = tex.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        tex.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool isBgra = tex.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                        tex.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                        tex.format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                        tex.format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    if (!isRgba && !isBgra) return false;
    if (tex.width == 0 || tex.height == 0) return false;
    if (tex.data.empty()) return false;

    outRgba.resize(static_cast<size_t>(tex.width) * tex.height * 4);
    for (UINT y = 0; y < tex.height; ++y) {
        const uint8_t* srcRow = tex.data.data() + static_cast<size_t>(y) * tex.rowPitch;
        uint8_t* dstRow = outRgba.data() + static_cast<size_t>(y) * tex.width * 4;
        for (UINT x = 0; x < tex.width; ++x) {
            const uint8_t* src = srcRow + x * 4;
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (isRgba) {
                r = src[0]; g = src[1]; b = src[2]; a = src[3];
            } else {
                b = src[0]; g = src[1]; r = src[2];
                a = (tex.format == DXGI_FORMAT_B8G8R8X8_UNORM || tex.format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
                    ? 255 : src[3];
            }
            if (flipGreen) g = static_cast<uint8_t>(255 - g);
            uint8_t* dst = dstRow + x * 4;
            dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
        }
    }
    return true;
}

static bool EncodePngRgba(const uint8_t* rgba, UINT width, UINT height, std::vector<uint8_t>& outPng) {
    if (!rgba || width == 0 || height == 0) return false;
    ComPtrGuard<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory.ptr)))) {
        return false;
    }
    ComPtrGuard<IWICBitmapEncoder> encoder;
    if (FAILED(factory.ptr->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder.ptr))) return false;
    ComPtrGuard<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream.ptr))) return false;
    if (FAILED(encoder.ptr->Initialize(stream.ptr, WICBitmapEncoderNoCache))) return false;
    ComPtrGuard<IWICBitmapFrameEncode> frame;
    ComPtrGuard<IPropertyBag2> props;
    if (FAILED(encoder.ptr->CreateNewFrame(&frame.ptr, &props.ptr))) return false;
    if (FAILED(frame.ptr->Initialize(props.ptr))) return false;
    if (FAILED(frame.ptr->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame.ptr->SetPixelFormat(&format))) return false;
    if (format != GUID_WICPixelFormat32bppRGBA) return false;
    const UINT stride = width * 4;
    const UINT imageSize = stride * height;
    if (FAILED(frame.ptr->WritePixels(height, stride, imageSize, const_cast<BYTE*>(rgba)))) return false;
    if (FAILED(frame.ptr->Commit())) return false;
    if (FAILED(encoder.ptr->Commit())) return false;

    HGLOBAL hglobal = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.ptr, &hglobal))) return false;
    const SIZE_T size = GlobalSize(hglobal);
    if (size == 0) return false;
    void* data = GlobalLock(hglobal);
    if (!data) return false;
    outPng.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
    GlobalUnlock(hglobal);
    return true;
}

static int AddressModeToGltf(D3D11_TEXTURE_ADDRESS_MODE mode) {
    switch (mode) {
    case D3D11_TEXTURE_ADDRESS_WRAP:
        return 10497; // REPEAT
    case D3D11_TEXTURE_ADDRESS_MIRROR:
    case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE:
        return 33648; // MIRRORED_REPEAT
    case D3D11_TEXTURE_ADDRESS_CLAMP:
    case D3D11_TEXTURE_ADDRESS_BORDER:
    default:
        return 33071; // CLAMP_TO_EDGE
    }
}

static void SamplerDescToGltf(const D3D11_SAMPLER_DESC& desc, int& outMin, int& outMag, int& outWrapS, int& outWrapT) {
    outWrapS = AddressModeToGltf(desc.AddressU);
    outWrapT = AddressModeToGltf(desc.AddressV);
    if (desc.Filter == D3D11_FILTER_ANISOTROPIC || desc.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC) {
        outMag = 9729;  // LINEAR
        outMin = 9987;  // LINEAR_MIPMAP_LINEAR
        return;
    }
    const D3D11_FILTER_TYPE minType = D3D11_DECODE_MIN_FILTER(desc.Filter);
    const D3D11_FILTER_TYPE magType = D3D11_DECODE_MAG_FILTER(desc.Filter);
    const D3D11_FILTER_TYPE mipType = D3D11_DECODE_MIP_FILTER(desc.Filter);
    outMag = (magType == D3D11_FILTER_TYPE_LINEAR) ? 9729 : 9728;
    if (mipType == D3D11_FILTER_TYPE_LINEAR) {
        outMin = (minType == D3D11_FILTER_TYPE_LINEAR) ? 9987 : 9986;
    } else {
        outMin = (minType == D3D11_FILTER_TYPE_LINEAR) ? 9985 : 9984;
    }
}

static void ConvertMatrixToColumnMajor(const float in[16], bool columnMajor, float out[16]) {
    if (columnMajor) {
        memcpy(out, in, sizeof(float) * 16);
        return;
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[c * 4 + r] = in[r * 4 + c];
        }
    }
}

static void ApplyFlipZToMatrix(float m[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float sign = (r == 2 ? -1.0f : 1.0f) * (c == 2 ? -1.0f : 1.0f);
            m[c * 4 + r] *= sign;
        }
    }
}

static void SaveFrameGltf(const FrameCapture& frame) {
    EnsureOutputDir();
    const uint64_t idx = frame.frameIndex;
    std::wstringstream ss;
    ss << L"frame_" << std::setfill(L'0') << std::setw(4) << idx << L".glb";
    fs::path path = g_outputDir / ss.str();
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        Log(L"Failed to open glTF file " + path.wstring());
        return;
    }

    struct BufferView {
        uint32_t byteOffset{0};
        uint32_t byteLength{0};
        uint32_t target{0};
    };
    struct Accessor {
        uint32_t bufferView{0};
        uint32_t byteOffset{0};
        uint32_t componentType{0};
        uint32_t count{0};
        std::string type;
        std::array<float, 3> min{{0, 0, 0}};
        std::array<float, 3> max{{0, 0, 0}};
        bool hasMinMax{false};
    };
    struct GeometryAccessors {
        int position{-1};
        int normal{-1};
        int tangent{-1};
        int uv{-1};
        int indices{-1};
        uint32_t vertexCount{0};
        uint32_t indexCount{0};
    };

    std::vector<uint8_t> bin;
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;
    std::vector<std::string> imagesJson;
    std::vector<std::string> samplersJson;
    std::vector<std::string> texturesJson;
    std::vector<std::string> materialsJson;
    std::vector<std::string> meshesJson;
    std::vector<std::string> nodesJson;
    std::vector<uint32_t> sceneNodes;

    auto pushBufferView = [&](uint32_t offset, uint32_t length, uint32_t target) {
        BufferView view;
        view.byteOffset = offset;
        view.byteLength = length;
        view.target = target;
        bufferViews.push_back(view);
        return static_cast<int>(bufferViews.size() - 1);
    };

    auto pushAccessor = [&](const Accessor& acc) {
        accessors.push_back(acc);
        return static_cast<int>(accessors.size() - 1);
    };

    for (const auto& sampler : frame.samplers) {
        int minFilter = 0;
        int magFilter = 0;
        int wrapS = 0;
        int wrapT = 0;
        SamplerDescToGltf(sampler.desc, minFilter, magFilter, wrapS, wrapT);
        std::ostringstream sj;
        sj << "{\"wrapS\":" << wrapS << ",\"wrapT\":" << wrapT
           << ",\"minFilter\":" << minFilter << ",\"magFilter\":" << magFilter << "}";
        samplersJson.push_back(sj.str());
    }

    std::unordered_map<uint64_t, int> imageCache;
    std::unordered_map<uint64_t, int> textureCache;
    auto getImageIndex = [&](size_t texIndex, bool flipGreen) -> int {
        uint64_t key = HashCombine(1469598103934665603ull, static_cast<uint64_t>(texIndex));
        key = HashCombine(key, static_cast<uint64_t>(flipGreen ? 1 : 0));
        auto it = imageCache.find(key);
        if (it != imageCache.end()) return it->second;
        if (texIndex >= frame.textures.size()) return -1;
        const auto& tex = frame.textures[texIndex];
        std::vector<uint8_t> rgba;
        if (!ConvertTextureToRgba(tex, rgba, flipGreen)) {
            LogOnce(LogOnceId::GltfUnsupportedTextureFormat, L"Unsupported texture format for glTF PNG export.");
            imageCache[key] = -1;
            return -1;
        }
        std::vector<uint8_t> png;
        if (!EncodePngRgba(rgba.data(), tex.width, tex.height, png)) {
            LogOnce(LogOnceId::GltfPngEncodeFailed, L"Failed to encode glTF PNG image.");
            imageCache[key] = -1;
            return -1;
        }
        const uint32_t offset = AppendToBin(bin, png.data(), png.size(), 4);
        const int viewIndex = pushBufferView(offset, static_cast<uint32_t>(png.size()), 0);
        std::ostringstream ij;
        ij << "{\"bufferView\":" << viewIndex << ",\"mimeType\":\"image/png\"}";
        const int imageIndex = static_cast<int>(imagesJson.size());
        imagesJson.push_back(ij.str());
        imageCache[key] = imageIndex;
        return imageIndex;
    };

    auto getTextureIndex = [&](size_t texIndex, int samplerIndex, bool flipGreen) -> int {
        const int imageIndex = getImageIndex(texIndex, flipGreen);
        if (imageIndex < 0) return -1;
        uint64_t key = HashCombine(1469598103934665603ull, static_cast<uint64_t>(imageIndex + 1));
        key = HashCombine(key, static_cast<uint64_t>(samplerIndex + 1));
        auto it = textureCache.find(key);
        if (it != textureCache.end()) return it->second;
        std::ostringstream tj;
        tj << "{\"source\":" << imageIndex;
        if (samplerIndex >= 0) tj << ",\"sampler\":" << samplerIndex;
        tj << "}";
        const int texOut = static_cast<int>(texturesJson.size());
        texturesJson.push_back(tj.str());
        textureCache[key] = texOut;
        return texOut;
    };

    std::unordered_map<uint64_t, GeometryAccessors> geometryCache;
    std::unordered_map<uint64_t, int> materialCache;

    const bool flipZ = g_gltfFlipZ.load();
    const bool flipWinding = g_gltfFlipWinding.load();
    const bool flipV = g_gltfFlipV.load();
    const bool flipNormalGreen = g_gltfFlipNormalGreen.load();
    const bool flipTangentW = g_gltfFlipTangentW.load();

    for (size_t drawIndex = 0; drawIndex < frame.draws.size(); ++drawIndex) {
        const auto& draw = frame.draws[drawIndex];
        if (draw.vertexCount == 0 || draw.indices.empty() || draw.vertexStreams.empty()) continue;

        uint64_t geomKey = HashInputLayout(draw.inputLayout);
        geomKey = HashCombine(geomKey, static_cast<uint64_t>(draw.topology));
        for (const auto& stream : draw.vertexStreams) {
            geomKey = HashCombine(geomKey, stream.slot);
            geomKey = HashCombine(geomKey, stream.stride);
            geomKey = HashBytes(stream.data.data(), stream.data.size(), geomKey);
        }
        geomKey = HashBytes(draw.indices.data(), draw.indices.size() * sizeof(uint32_t), geomKey);

        GeometryAccessors geom;
        bool geomFound = false;
        if (g_gltfDedupMeshes.load()) {
            auto it = geometryCache.find(geomKey);
            if (it != geometryCache.end()) {
                geom = it->second;
                geomFound = true;
            }
        }

        if (!geomFound) {
            const InputElementInfo* posElem = FindElementAnySlot(draw.inputLayout, "POSITION", 0);
            const InputElementInfo* normElem = FindElementAnySlot(draw.inputLayout, "NORMAL", 0);
            const InputElementInfo* uvElem = FindElementAnySlot(draw.inputLayout, "TEXCOORD", 0);
            const InputElementInfo* tanElem = FindElementAnySlot(draw.inputLayout, "TANGENT", 0);
            if (!posElem) continue;
            const VertexStreamCapture* posStream = FindVertexStream(draw.vertexStreams, posElem->slot);
            if (!posStream) continue;

            const size_t vertexCount = draw.vertexCount;
            std::vector<float> positions(vertexCount * 3);
            std::vector<float> normals;
            std::vector<float> tangents;
            std::vector<float> uvs;
            if (normElem) normals.resize(vertexCount * 3);
            if (tanElem) tangents.resize(vertexCount * 4);
            if (uvElem) uvs.resize(vertexCount * 2);

            std::array<float, 3> minPos{{ std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max() }};
            std::array<float, 3> maxPos{{ -std::numeric_limits<float>::max(),
                                           -std::numeric_limits<float>::max(),
                                           -std::numeric_limits<float>::max() }};

            auto decodeElem = [&](const InputElementInfo* elem, const VertexStreamCapture* stream,
                                  size_t vertexIndex, float outVals[4], UINT& components) -> bool {
                if (!elem || !stream) return false;
                if (elem->offset + FormatSizeBytes(elem->format) > stream->stride) return false;
                const uint8_t* base = stream->data.data() + vertexIndex * stream->stride;
                return DecodeDXGIFormat(elem->format, base + elem->offset, outVals, components);
            };

            const VertexStreamCapture* normStream = normElem ? FindVertexStream(draw.vertexStreams, normElem->slot) : nullptr;
            const VertexStreamCapture* uvStream = uvElem ? FindVertexStream(draw.vertexStreams, uvElem->slot) : nullptr;
            const VertexStreamCapture* tanStream = tanElem ? FindVertexStream(draw.vertexStreams, tanElem->slot) : nullptr;

            for (size_t i = 0; i < vertexCount; ++i) {
                float decoded[4]{};
                UINT components = 0;
                if (!decodeElem(posElem, posStream, i, decoded, components) || components < 3) {
                    decoded[0] = decoded[1] = decoded[2] = 0.0f;
                }
                float px = decoded[0];
                float py = decoded[1];
                float pz = decoded[2];
                if (flipZ) pz = -pz;
                positions[i * 3 + 0] = px;
                positions[i * 3 + 1] = py;
                positions[i * 3 + 2] = pz;
                minPos[0] = std::min(minPos[0], px);
                minPos[1] = std::min(minPos[1], py);
                minPos[2] = std::min(minPos[2], pz);
                maxPos[0] = std::max(maxPos[0], px);
                maxPos[1] = std::max(maxPos[1], py);
                maxPos[2] = std::max(maxPos[2], pz);

                if (normElem && normStream) {
                    if (decodeElem(normElem, normStream, i, decoded, components) && components >= 3) {
                        float nx = decoded[0];
                        float ny = decoded[1];
                        float nz = decoded[2];
                        if (flipZ) nz = -nz;
                        normals[i * 3 + 0] = nx;
                        normals[i * 3 + 1] = ny;
                        normals[i * 3 + 2] = nz;
                    } else {
                        normals[i * 3 + 0] = 0.0f;
                        normals[i * 3 + 1] = 0.0f;
                        normals[i * 3 + 2] = 0.0f;
                    }
                }

                if (tanElem && tanStream) {
                    if (decodeElem(tanElem, tanStream, i, decoded, components) && components >= 3) {
                        float tx = decoded[0];
                        float ty = decoded[1];
                        float tz = decoded[2];
                        float tw = components >= 4 ? decoded[3] : 1.0f;
                        if (flipZ) tz = -tz;
                        if (flipTangentW) tw = -tw;
                        tangents[i * 4 + 0] = tx;
                        tangents[i * 4 + 1] = ty;
                        tangents[i * 4 + 2] = tz;
                        tangents[i * 4 + 3] = tw;
                    } else {
                        tangents[i * 4 + 0] = 0.0f;
                        tangents[i * 4 + 1] = 0.0f;
                        tangents[i * 4 + 2] = 0.0f;
                        tangents[i * 4 + 3] = 1.0f;
                    }
                }

                if (uvElem && uvStream) {
                    if (decodeElem(uvElem, uvStream, i, decoded, components) && components >= 2) {
                        float u = decoded[0];
                        float v = decoded[1];
                        if (flipV) v = 1.0f - v;
                        uvs[i * 2 + 0] = u;
                        uvs[i * 2 + 1] = v;
                    } else {
                        uvs[i * 2 + 0] = 0.0f;
                        uvs[i * 2 + 1] = 0.0f;
                    }
                }
            }

            std::vector<uint32_t> indices = draw.indices;
            if (flipWinding && indices.size() >= 3) {
                for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                    std::swap(indices[i + 1], indices[i + 2]);
                }
            }

            const uint32_t maxIndex = indices.empty() ? 0 : *std::max_element(indices.begin(), indices.end());
            const bool useShort = maxIndex <= 0xFFFF;

            const uint32_t posOffset = AppendToBin(bin, positions.data(), positions.size() * sizeof(float), 4);
            const int posView = pushBufferView(posOffset, static_cast<uint32_t>(positions.size() * sizeof(float)), 34962);
            Accessor posAcc;
            posAcc.bufferView = posView;
            posAcc.byteOffset = 0;
            posAcc.componentType = 5126;
            posAcc.count = static_cast<uint32_t>(vertexCount);
            posAcc.type = "VEC3";
            posAcc.min = minPos;
            posAcc.max = maxPos;
            posAcc.hasMinMax = true;
            geom.position = pushAccessor(posAcc);

            if (!normals.empty()) {
                const uint32_t normOffset = AppendToBin(bin, normals.data(), normals.size() * sizeof(float), 4);
                const int normView = pushBufferView(normOffset, static_cast<uint32_t>(normals.size() * sizeof(float)), 34962);
                Accessor normAcc;
                normAcc.bufferView = normView;
                normAcc.byteOffset = 0;
                normAcc.componentType = 5126;
                normAcc.count = static_cast<uint32_t>(vertexCount);
                normAcc.type = "VEC3";
                geom.normal = pushAccessor(normAcc);
            }
            if (!tangents.empty()) {
                const uint32_t tanOffset = AppendToBin(bin, tangents.data(), tangents.size() * sizeof(float), 4);
                const int tanView = pushBufferView(tanOffset, static_cast<uint32_t>(tangents.size() * sizeof(float)), 34962);
                Accessor tanAcc;
                tanAcc.bufferView = tanView;
                tanAcc.byteOffset = 0;
                tanAcc.componentType = 5126;
                tanAcc.count = static_cast<uint32_t>(vertexCount);
                tanAcc.type = "VEC4";
                geom.tangent = pushAccessor(tanAcc);
            }
            if (!uvs.empty()) {
                const uint32_t uvOffset = AppendToBin(bin, uvs.data(), uvs.size() * sizeof(float), 4);
                const int uvView = pushBufferView(uvOffset, static_cast<uint32_t>(uvs.size() * sizeof(float)), 34962);
                Accessor uvAcc;
                uvAcc.bufferView = uvView;
                uvAcc.byteOffset = 0;
                uvAcc.componentType = 5126;
                uvAcc.count = static_cast<uint32_t>(vertexCount);
                uvAcc.type = "VEC2";
                geom.uv = pushAccessor(uvAcc);
            }

            if (useShort) {
                std::vector<uint16_t> shortIndices(indices.size());
                for (size_t i = 0; i < indices.size(); ++i) shortIndices[i] = static_cast<uint16_t>(indices[i]);
                const uint32_t idxOffset = AppendToBin(bin, shortIndices.data(), shortIndices.size() * sizeof(uint16_t), 4);
                const int idxView = pushBufferView(idxOffset, static_cast<uint32_t>(shortIndices.size() * sizeof(uint16_t)), 34963);
                Accessor idxAcc;
                idxAcc.bufferView = idxView;
                idxAcc.byteOffset = 0;
                idxAcc.componentType = 5123;
                idxAcc.count = static_cast<uint32_t>(shortIndices.size());
                idxAcc.type = "SCALAR";
                geom.indices = pushAccessor(idxAcc);
                geom.indexCount = static_cast<uint32_t>(shortIndices.size());
            } else {
                const uint32_t idxOffset = AppendToBin(bin, indices.data(), indices.size() * sizeof(uint32_t), 4);
                const int idxView = pushBufferView(idxOffset, static_cast<uint32_t>(indices.size() * sizeof(uint32_t)), 34963);
                Accessor idxAcc;
                idxAcc.bufferView = idxView;
                idxAcc.byteOffset = 0;
                idxAcc.componentType = 5125;
                idxAcc.count = static_cast<uint32_t>(indices.size());
                idxAcc.type = "SCALAR";
                geom.indices = pushAccessor(idxAcc);
                geom.indexCount = static_cast<uint32_t>(indices.size());
            }

            geom.vertexCount = static_cast<uint32_t>(vertexCount);
            geometryCache[geomKey] = geom;
        }

        if (geom.position < 0 || geom.indices < 0) continue;

        const int baseSampler = draw.samplerBindings[0];
        const int normalSampler = draw.samplerBindings[1];
        const int baseTex = draw.srvTextures[0] >= 0
            ? getTextureIndex(static_cast<size_t>(draw.srvTextures[0]), baseSampler, false)
            : -1;
        const int normalTex = draw.srvTextures[1] >= 0
            ? getTextureIndex(static_cast<size_t>(draw.srvTextures[1]), normalSampler, flipNormalGreen)
            : -1;

        uint64_t matKey = HashCombine(1469598103934665603ull, static_cast<uint64_t>(baseTex + 1));
        matKey = HashCombine(matKey, static_cast<uint64_t>(normalTex + 1));
        int materialIndex = -1;
        auto itMat = materialCache.find(matKey);
        if (itMat != materialCache.end()) {
            materialIndex = itMat->second;
        } else if (baseTex >= 0 || normalTex >= 0) {
            std::ostringstream mj;
            mj << "{";
            bool wrote = false;
            if (baseTex >= 0) {
                mj << "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" << baseTex << "}}";
                wrote = true;
            }
            if (normalTex >= 0) {
                if (wrote) mj << ",";
                mj << "\"normalTexture\":{\"index\":" << normalTex << "}";
            }
            mj << "}";
            materialIndex = static_cast<int>(materialsJson.size());
            materialsJson.push_back(mj.str());
            materialCache[matKey] = materialIndex;
        }

        std::ostringstream meshj;
        meshj << "{\"primitives\":[{\"attributes\":{";
        bool firstAttr = true;
        auto addAttr = [&](const char* name, int accIndex) {
            if (accIndex < 0) return;
            if (!firstAttr) meshj << ",";
            firstAttr = false;
            meshj << "\"" << name << "\":" << accIndex;
        };
        addAttr("POSITION", geom.position);
        addAttr("NORMAL", geom.normal);
        addAttr("TANGENT", geom.tangent);
        addAttr("TEXCOORD_0", geom.uv);
        meshj << "},\"indices\":" << geom.indices;
        if (materialIndex >= 0) meshj << ",\"material\":" << materialIndex;
        meshj << "}]}";
        const int meshIndex = static_cast<int>(meshesJson.size());
        meshesJson.push_back(meshj.str());

        std::ostringstream extras;
        extras << "\"extras\":{";
        bool firstExtra = true;
        auto addExtra = [&](const std::string& key, const std::string& value) {
            if (!firstExtra) extras << ",";
            firstExtra = false;
            extras << "\"" << key << "\":" << value;
        };
        addExtra("draw_index", std::to_string(drawIndex));
        addExtra("instance_count", std::to_string(draw.instanceCount));
        addExtra("topology", std::to_string(static_cast<uint32_t>(draw.topology)));

        if (!draw.vsConstantBuffers.empty()) {
            std::ostringstream cb;
            cb << "[";
            bool firstCb = true;
            for (const auto& buf : draw.vsConstantBuffers) {
                if (!firstCb) cb << ",";
                firstCb = false;
                const uint32_t offset = AppendToBin(bin, buf.data.data(), buf.data.size(), 4);
                const int viewIndex = pushBufferView(offset, static_cast<uint32_t>(buf.data.size()), 0);
                cb << "{\"slot\":" << buf.slot << ",\"bufferView\":" << viewIndex
                   << ",\"byteLength\":" << buf.data.size() << "}";
            }
            cb << "]";
            addExtra("vs_cbuffers", cb.str());
        }
        if (!draw.psConstantBuffers.empty()) {
            std::ostringstream cb;
            cb << "[";
            bool firstCb = true;
            for (const auto& buf : draw.psConstantBuffers) {
                if (!firstCb) cb << ",";
                firstCb = false;
                const uint32_t offset = AppendToBin(bin, buf.data.data(), buf.data.size(), 4);
                const int viewIndex = pushBufferView(offset, static_cast<uint32_t>(buf.data.size()), 0);
                cb << "{\"slot\":" << buf.slot << ",\"bufferView\":" << viewIndex
                   << ",\"byteLength\":" << buf.data.size() << "}";
            }
            cb << "]";
            addExtra("ps_cbuffers", cb.str());
        }
        if (!draw.instanceStreams.empty()) {
            std::ostringstream inst;
            inst << "[";
            bool firstInst = true;
            for (const auto& buf : draw.instanceStreams) {
                if (!firstInst) inst << ",";
                firstInst = false;
                const uint32_t offset = AppendToBin(bin, buf.data.data(), buf.data.size(), 4);
                const int viewIndex = pushBufferView(offset, static_cast<uint32_t>(buf.data.size()), 0);
                inst << "{\"slot\":" << buf.slot << ",\"bufferView\":" << viewIndex
                     << ",\"byteLength\":" << buf.data.size()
                     << ",\"stride\":" << buf.stride
                     << ",\"count\":" << buf.instanceCount
                     << ",\"start\":" << buf.startInstance << "}";
            }
            inst << "]";
            addExtra("instance_buffers", inst.str());
        }
        {
            std::ostringstream srvs;
            srvs << "[";
            bool firstSrv = true;
            for (UINT slot = 0; slot < kMaxFrameSrvs; ++slot) {
                const int texBinding = draw.srvTextures[slot];
                if (texBinding < 0) continue;
                if (!firstSrv) srvs << ",";
                firstSrv = false;
                const bool useFlipGreen = (slot == 1 && flipNormalGreen);
                const int texIndex = getTextureIndex(static_cast<size_t>(texBinding),
                                                     draw.samplerBindings[slot], useFlipGreen);
                srvs << "{\"slot\":" << slot;
                if (texIndex >= 0) srvs << ",\"texture\":" << texIndex;
                else srvs << ",\"texture\":null";
                if (static_cast<size_t>(texBinding) < frame.textures.size()) {
                    const auto& meta = frame.textures[texBinding];
                    srvs << ",\"width\":" << meta.width
                         << ",\"height\":" << meta.height
                         << ",\"format\":" << static_cast<uint32_t>(meta.format)
                         << ",\"srgb\":" << (meta.isSrgb ? "true" : "false");
                }
                srvs << "}";
            }
            srvs << "]";
            addExtra("srvs", srvs.str());
        }
        {
            std::ostringstream sams;
            sams << "[";
            bool firstSam = true;
            for (UINT slot = 0; slot < kMaxFrameSamplers; ++slot) {
                const int samplerIndex = draw.samplerBindings[slot];
                if (samplerIndex < 0) continue;
                if (!firstSam) sams << ",";
                firstSam = false;
                sams << "{\"slot\":" << slot << ",\"sampler\":" << samplerIndex << "}";
            }
            sams << "]";
            addExtra("samplers", sams.str());
        }
        if (draw.hasTransform && draw.transformSource.valid) {
            std::ostringstream ms;
            ms << "{\"stage\":\"vs\",\"slot\":" << draw.transformSource.slot
               << ",\"offset\":" << draw.transformSource.offsetBytes
               << ",\"columnMajor\":" << (draw.transformSource.columnMajor ? "true" : "false") << "}";
            addExtra("matrix_source", ms.str());
        }
        extras << "}";

        std::ostringstream nodej;
        nodej << "{\"mesh\":" << meshIndex;
        nodej << ",\"name\":\"draw_" << drawIndex << "\"";
        if (draw.hasTransform) {
            float matrix[16]{};
            ConvertMatrixToColumnMajor(draw.transform, draw.transformSource.columnMajor, matrix);
            if (flipZ) {
                ApplyFlipZToMatrix(matrix);
            }
            nodej << ",\"matrix\":[";
            for (int i = 0; i < 16; ++i) {
                if (i) nodej << ",";
                nodej << matrix[i];
            }
            nodej << "]";
        }
        nodej << "," << extras.str();
        nodej << "}";
        const int nodeIndex = static_cast<int>(nodesJson.size());
        nodesJson.push_back(nodej.str());
        sceneNodes.push_back(static_cast<uint32_t>(nodeIndex));
    }

    std::ostringstream json;
    json << "{";
    json << "\"asset\":{\"version\":\"2.0\",\"generator\":\"AssetRipper\"}";
    json << ",\"buffers\":[{\"byteLength\":" << bin.size() << "}]";
    json << ",\"bufferViews\":[";
    for (size_t i = 0; i < bufferViews.size(); ++i) {
        if (i) json << ",";
        const auto& view = bufferViews[i];
        json << "{\"buffer\":0,\"byteOffset\":" << view.byteOffset
             << ",\"byteLength\":" << view.byteLength;
        if (view.target != 0) json << ",\"target\":" << view.target;
        json << "}";
    }
    json << "]";
    json << ",\"accessors\":[";
    for (size_t i = 0; i < accessors.size(); ++i) {
        if (i) json << ",";
        const auto& acc = accessors[i];
        json << "{\"bufferView\":" << acc.bufferView
             << ",\"byteOffset\":" << acc.byteOffset
             << ",\"componentType\":" << acc.componentType
             << ",\"count\":" << acc.count
             << ",\"type\":\"" << acc.type << "\"";
        if (acc.hasMinMax) {
            json << ",\"min\":[" << acc.min[0] << "," << acc.min[1] << "," << acc.min[2] << "]";
            json << ",\"max\":[" << acc.max[0] << "," << acc.max[1] << "," << acc.max[2] << "]";
        }
        json << "}";
    }
    json << "]";
    json << ",\"images\":[";
    for (size_t i = 0; i < imagesJson.size(); ++i) {
        if (i) json << ",";
        json << imagesJson[i];
    }
    json << "]";
    json << ",\"samplers\":[";
    for (size_t i = 0; i < samplersJson.size(); ++i) {
        if (i) json << ",";
        json << samplersJson[i];
    }
    json << "]";
    json << ",\"textures\":[";
    for (size_t i = 0; i < texturesJson.size(); ++i) {
        if (i) json << ",";
        json << texturesJson[i];
    }
    json << "]";
    json << ",\"materials\":[";
    for (size_t i = 0; i < materialsJson.size(); ++i) {
        if (i) json << ",";
        json << materialsJson[i];
    }
    json << "]";
    json << ",\"meshes\":[";
    for (size_t i = 0; i < meshesJson.size(); ++i) {
        if (i) json << ",";
        json << meshesJson[i];
    }
    json << "]";
    json << ",\"nodes\":[";
    for (size_t i = 0; i < nodesJson.size(); ++i) {
        if (i) json << ",";
        json << nodesJson[i];
    }
    json << "]";
    json << ",\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        if (i) json << ",";
        json << sceneNodes[i];
    }
    json << "]}]";
    json << ",\"scene\":0";
    json << "}";

    std::string jsonStr = json.str();
    const size_t jsonPadding = (4 - (jsonStr.size() % 4)) % 4;
    jsonStr.append(jsonPadding, ' ');
    const size_t binPadding = (4 - (bin.size() % 4)) % 4;
    bin.insert(bin.end(), binPadding, 0);

    const uint32_t jsonChunkLength = static_cast<uint32_t>(jsonStr.size());
    const uint32_t binChunkLength = static_cast<uint32_t>(bin.size());
    const uint32_t totalLength = 12 + 8 + jsonChunkLength + 8 + binChunkLength;

    const uint32_t magic = 0x46546C67; // "glTF"
    const uint32_t version = 2;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&totalLength), sizeof(totalLength));

    const uint32_t jsonChunkType = 0x4E4F534A; // "JSON"
    out.write(reinterpret_cast<const char*>(&jsonChunkLength), sizeof(jsonChunkLength));
    out.write(reinterpret_cast<const char*>(&jsonChunkType), sizeof(jsonChunkType));
    out.write(jsonStr.data(), static_cast<std::streamsize>(jsonStr.size()));

    const uint32_t binChunkType = 0x004E4942; // "BIN"
    out.write(reinterpret_cast<const char*>(&binChunkLength), sizeof(binChunkLength));
    out.write(reinterpret_cast<const char*>(&binChunkType), sizeof(binChunkType));
    if (!bin.empty()) {
        out.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    }

    UpdateOverlay(L"Saved frame glTF", ss.str());
}

static void WriterThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);
    while (true) {
        std::unique_lock<std::mutex> lock(g_queueMutex);
        g_queueCv.wait(lock, [] { return g_shutdown.load() || !g_queue.empty(); });
        if (g_queue.empty()) {
            if (g_shutdown.load()) break;
            continue;
        }
        const size_t itemBytes = EstimateItemBytes(g_queue.front());
        auto item = std::move(g_queue.front());
        g_queue.pop_front();
        if (g_queueBytes >= itemBytes) g_queueBytes -= itemBytes;
        lock.unlock();

        switch (item.type) {
        case CaptureType::Mesh: SaveMesh(item); break;
        case CaptureType::Texture: SaveTexture(item); break;
        case CaptureType::Shader: SaveShader(item); break;
        case CaptureType::Frame:
            if (item.frame) {
                // glTF export is only for explicit frame captures.
                SaveFrameGltf(*item.frame);
            }
            break;
        }
    }
    if (comInit) CoUninitialize();
}

static bool ParseUInt32(const std::string& value, uint32_t& out) {
    if (value.empty()) return false;
    try {
        size_t idx = 0;
        const unsigned long v = std::stoul(value, &idx, 10);
        if (idx != value.size()) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

static void LoadConfig() {
    fs::path cfg = GetConfigPath();
    if (!fs::exists(cfg)) return;
    std::ifstream in(cfg, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        auto key = line.substr(0, pos);
        auto value = line.substr(pos + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();
        if (key == "output_dir") {
            g_outputDir = fs::path(Utf8ToWide(value));
        } else if (key == "auto_capture") {
            SetAutoCaptureEnabled(value == "1");
        } else if (key == "auto_capture_draws") {
            uint32_t parsed = 0;
            if (ParseUInt32(value, parsed)) {
                g_autoCaptureMaxPerWindow.store(parsed);
            }
        } else if (key == "auto_capture_seconds") {
            uint32_t parsed = 0;
            if (ParseUInt32(value, parsed)) {
                g_autoCaptureWindowMs.store(parsed * 1000);
            }
        } else if (key == "overlay") {
            g_overlayEnabled.store(value != "0");
        } else if (key == "capture_frame") {
            if (value == "1") {
                RequestFrameCapture(L"Config capture_frame requested.");
            }
        } else if (key == "gltf_flip_z") {
            g_gltfFlipZ.store(value != "0");
        } else if (key == "gltf_flip_winding") {
            g_gltfFlipWinding.store(value != "0");
        } else if (key == "gltf_flip_v") {
            g_gltfFlipV.store(value != "0");
        } else if (key == "gltf_flip_normal_green") {
            g_gltfFlipNormalGreen.store(value != "0");
        } else if (key == "gltf_flip_tangent_w") {
            g_gltfFlipTangentW.store(value != "0");
        } else if (key == "gltf_dedup_meshes") {
            g_gltfDedupMeshes.store(value != "0");
        } else if (key == "gltf_dedup_textures") {
            g_gltfDedupTextures.store(value != "0");
        } else if (key == "gltf_dedup_samplers") {
            g_gltfDedupSamplers.store(value != "0");
        }
    }
    if (g_outputDir.empty() && g_hModule) {
        wchar_t modulePath[MAX_PATH];
        GetModuleFileNameW(g_hModule, modulePath, MAX_PATH);
        fs::path base(modulePath);
        g_outputDir = base.parent_path() / "captures";
    }
}

// D3D9 capturing
static void CaptureD3D9(IDirect3DDevice9* device, INT baseVertexIndex, UINT startIndex, UINT indexCount) {
    if (!g_captureRequested.load() && !g_captureEveryFrame.load()) return;
    if (g_shutdown.load()) return;
    if (!g_captureRequested.load() && g_captureEveryFrame.load() && !AllowAutoCapture()) return;
    thread_local bool reentry = false;
    if (reentry) return;
    reentry = true;

    ComPtrGuard<IDirect3DVertexBuffer9> vb;
    UINT stride = 0, offset = 0;
    if (FAILED(device->GetStreamSource(0, &vb.ptr, &offset, &stride)) || !vb.ptr || stride == 0) {
        reentry = false;
        return;
    }
    (void)offset;

    ComPtrGuard<IDirect3DIndexBuffer9> ib;
    D3DFORMAT idxFmt = D3DFMT_UNKNOWN;
    device->GetIndices(&ib.ptr);
    if (ib.ptr) {
        D3DINDEXBUFFER_DESC ibDesc{};
        ib.ptr->GetDesc(&ibDesc);
        idxFmt = ibDesc.Format;
    }
    if (ib.ptr && idxFmt != D3DFMT_INDEX16 && idxFmt != D3DFMT_INDEX32) {
        LogOnce(LogOnceId::UnsupportedIndexFormat, L"Unsupported D3D9 index buffer format; skipping capture.");
        reentry = false;
        return;
    }

    D3DVERTEXBUFFER_DESC vbDesc{};
    vb.ptr->GetDesc(&vbDesc);
    const UINT totalVertexCount = vbDesc.Size / stride;
    if (totalVertexCount < 3 || stride == 0) {
        reentry = false;
        return;
    }

    std::vector<InputElementInfo> inputLayout;
    ComPtrGuard<IDirect3DVertexDeclaration9> decl;
    if (SUCCEEDED(device->GetVertexDeclaration(&decl.ptr)) && decl.ptr) {
        UINT elemCount = 0;
        if (SUCCEEDED(decl.ptr->GetDeclaration(nullptr, &elemCount)) && elemCount > 0) {
            std::vector<D3DVERTEXELEMENT9> elems(elemCount);
            if (SUCCEEDED(decl.ptr->GetDeclaration(elems.data(), &elemCount))) {
                for (const auto& elem : elems) {
                    if (elem.Stream == 0xFF && elem.Type == D3DDECLTYPE_UNUSED) break;
                    if (elem.Stream != 0) continue;
                    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
                    if (!D3D9DeclTypeToDXGI(elem.Type, fmt)) continue;
                    InputElementInfo info{};
                    info.semantic = D3D9UsageToSemantic(elem.Usage);
                    if (info.semantic.empty()) continue;
                    info.semanticIndex = elem.UsageIndex;
                    info.format = fmt;
                    info.slot = elem.Stream;
                    info.offset = elem.Offset;
                    inputLayout.push_back(info);
                }
            }
        }
    }
    if (inputLayout.empty()) {
        LogOnce(LogOnceId::D3D9VertexDeclMissing, L"D3D9 vertex declaration missing or unsupported; skipping OBJ.");
        reentry = false;
        return;
    }

    if (!ib.ptr || indexCount == 0) {
        reentry = false;
        return;
    }

    std::vector<uint32_t> indices;
    uint32_t minIndex = UINT32_MAX;
    uint32_t maxIndex = 0;
    void* ibData = nullptr;
    if (SUCCEEDED(ib.ptr->Lock(0, 0, &ibData, D3DLOCK_READONLY))) {
        const UINT indexStride = (idxFmt == D3DFMT_INDEX32) ? 4 : 2;
        const UINT totalIndices = indexCount;
        D3DINDEXBUFFER_DESC ibDesc{};
        ib.ptr->GetDesc(&ibDesc);
        const UINT available = ibDesc.Size / indexStride;
        if (startIndex + totalIndices > available) {
            ib.ptr->Unlock();
            reentry = false;
            return;
        }
        indices.resize(totalIndices);
        for (UINT i = 0; i < totalIndices; ++i) {
            uint32_t idx = 0;
            if (indexStride == 4) {
                idx = reinterpret_cast<const uint32_t*>(ibData)[startIndex + i];
            } else {
                idx = reinterpret_cast<const uint16_t*>(ibData)[startIndex + i];
            }
            const int64_t actual = static_cast<int64_t>(idx) + static_cast<int64_t>(baseVertexIndex);
            if (actual < 0 || static_cast<uint64_t>(actual) >= totalVertexCount) {
                ib.ptr->Unlock();
                LogOnce(LogOnceId::InvalidVertexRange, L"D3D9 index range outside vertex buffer; skipping capture.");
                reentry = false;
                return;
            }
            const uint32_t actualIndex = static_cast<uint32_t>(actual);
            minIndex = std::min(minIndex, actualIndex);
            maxIndex = std::max(maxIndex, actualIndex);
            indices[i] = actualIndex;
        }
        ib.ptr->Unlock();
    }

    if (minIndex > maxIndex || maxIndex >= totalVertexCount) {
        reentry = false;
        return;
    }

    const uint32_t vertexCount = maxIndex - minIndex + 1;
    const size_t startByte = static_cast<size_t>(offset) + static_cast<size_t>(minIndex) * stride;
    const size_t byteCount = static_cast<size_t>(vertexCount) * stride;
    if (startByte + byteCount > vbDesc.Size) {
        LogOnce(LogOnceId::InvalidVertexRange, L"D3D9 vertex range exceeds buffer; skipping capture.");
        reentry = false;
        return;
    }

    void* vbData = nullptr;
    if (FAILED(vb.ptr->Lock(static_cast<UINT>(startByte), static_cast<UINT>(byteCount), &vbData, D3DLOCK_READONLY))) {
        reentry = false;
        return;
    }

    CaptureItem item;
    item.type = CaptureType::Mesh;
    item.stride = stride;
    item.vertexCount = vertexCount;
    item.vertexSlot = 0;
    item.inputLayout = std::move(inputLayout);
    item.vertexData.resize(byteCount);
    memcpy(item.vertexData.data(), vbData, byteCount);
    vb.ptr->Unlock();

    for (auto& idx : indices) {
        idx -= minIndex;
    }
    item.indices = std::move(indices);

    Enqueue(std::move(item));

    // Capture shaders (bytecode dump to .dxbc)
    ComPtrGuard<IDirect3DVertexShader9> vs;
    if (SUCCEEDED(device->GetVertexShader(&vs.ptr)) && vs.ptr) {
        UINT size = 0;
        if (SUCCEEDED(vs.ptr->GetFunction(nullptr, &size)) && size > 0) {
            std::vector<uint8_t> data(size);
            if (SUCCEEDED(vs.ptr->GetFunction(data.data(), &size))) {
                CaptureItem shaderItem;
                shaderItem.type = CaptureType::Shader;
                shaderItem.shaderBytes = std::move(data);
                shaderItem.shaderStage = ShaderStage::Vertex;
                Enqueue(std::move(shaderItem));
            }
        }
    }
    ComPtrGuard<IDirect3DPixelShader9> ps;
    if (SUCCEEDED(device->GetPixelShader(&ps.ptr)) && ps.ptr) {
        UINT size = 0;
        if (SUCCEEDED(ps.ptr->GetFunction(nullptr, &size)) && size > 0) {
            std::vector<uint8_t> data(size);
            if (SUCCEEDED(ps.ptr->GetFunction(data.data(), &size))) {
                CaptureItem shaderItem;
                shaderItem.type = CaptureType::Shader;
                shaderItem.shaderBytes = std::move(data);
                shaderItem.shaderStage = ShaderStage::Pixel;
                Enqueue(std::move(shaderItem));
            }
        }
    }

    // Capture first bound texture (stage 0)
    ComPtrGuard<IDirect3DBaseTexture9> texBase;
    for (DWORD stage = 0; stage < 2; ++stage) {
        if (SUCCEEDED(device->GetTexture(stage, &texBase.ptr)) && texBase.ptr) {
            if (texBase.ptr->GetType() == D3DRTYPE_TEXTURE) {
                IDirect3DTexture9* tex = static_cast<IDirect3DTexture9*>(texBase.ptr);
                const UINT levelCount = tex->GetLevelCount();
                if (levelCount > 0) {
                    D3DSURFACE_DESC desc{};
                    if (FAILED(tex->GetLevelDesc(0, &desc))) {
                        continue;
                    }
                    const DXGI_FORMAT fmt = D3D9FormatToDXGI(desc.Format);
                    if (fmt == DXGI_FORMAT_UNKNOWN || IsTypelessFormat(fmt)) {
                        LogOnce(LogOnceId::TypelessTextureFormat, L"D3D9 texture format unsupported; skipping DDS.");
                        continue;
                    }
                    CaptureItem texItem;
                    texItem.type = CaptureType::Texture;
                    texItem.texWidth = desc.Width;
                    texItem.texHeight = desc.Height;
                    texItem.texFormat = fmt;
                    texItem.texRowPitch = static_cast<UINT>(CalcRowPitch(fmt, desc.Width));
                    texItem.texMipLevels = levelCount;
                    texItem.texArraySize = 1;
                    texItem.texIsCube = false;
                    texItem.texDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
                    size_t totalSize = 0;
                    for (UINT level = 0; level < levelCount; ++level) {
                        D3DSURFACE_DESC levelDesc{};
                        if (SUCCEEDED(tex->GetLevelDesc(level, &levelDesc))) {
                            totalSize += CalcSubresourceSize(fmt, levelDesc.Width, levelDesc.Height);
                        }
                    }
                    texItem.textureData.reserve(totalSize);

                    for (UINT level = 0; level < levelCount; ++level) {
                        D3DSURFACE_DESC levelDesc{};
                        if (FAILED(tex->GetLevelDesc(level, &levelDesc))) continue;
                        D3DLOCKED_RECT lr{};
                        if (FAILED(tex->LockRect(level, &lr, nullptr, D3DLOCK_READONLY))) continue;
                        const size_t rowSize = CalcRowPitch(fmt, levelDesc.Width);
                        const size_t rowCount = CalcRowCount(fmt, levelDesc.Height);
                        const uint8_t* src = reinterpret_cast<const uint8_t*>(lr.pBits);
                        for (size_t row = 0; row < rowCount; ++row) {
                            texItem.textureData.insert(texItem.textureData.end(),
                                                       src + row * lr.Pitch,
                                                       src + row * lr.Pitch + rowSize);
                        }
                        tex->UnlockRect(level);
                    }
                    Enqueue(std::move(texItem));
                }
            }
        }
        texBase.ptr = nullptr;
    }

    g_captureRequested.store(false);
    reentry = false;
}

static void PopulateInputLayout(ID3D11DeviceContext* ctx, UINT slot, std::vector<InputElementInfo>& outLayout) {
    if (!ctx) return;
    ComPtrGuard<ID3D11InputLayout> layout;
    ctx->IAGetInputLayout(&layout.ptr);
    if (!layout.ptr) return;
    void* key = layout.ptr;
    ComPtrGuard<IUnknown> identity;
    if (SUCCEEDED(layout.ptr->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity.ptr))) &&
        identity.ptr) {
        key = identity.ptr;
    }
    std::lock_guard<std::mutex> lock(g_layoutMutex);
    auto it = g_layouts.find(key);
    if (it != g_layouts.end()) {
        outLayout = it->second;
    }
}

// D3D11 capturing
static bool CaptureD3D11Mesh(ID3D11DeviceContext* ctx, UINT vertexSlot, ID3D11Buffer* vb,
                             UINT stride, UINT offsetBytes, uint32_t minIndex, uint32_t maxIndex,
                             std::vector<uint32_t>&& indices) {
    if (!ctx || !vb || stride == 0) return false;
    ComPtrGuard<ID3D11Device> device;
    ctx->GetDevice(&device.ptr);
    if (!device.ptr) return false;
    if (minIndex > maxIndex) return false;
    const uint32_t vertexCount = maxIndex - minIndex + 1;
    std::vector<uint8_t> vertexData;
    if (!ReadVertexStreamRange(ctx, device.ptr, vb, stride, offsetBytes, minIndex, maxIndex, vertexData)) {
        LogOnce(LogOnceId::MapStagingVBFailed, L"Failed to read vertex buffer range.");
        return false;
    }

    CaptureItem item;
    item.type = CaptureType::Mesh;
    item.stride = stride;
    item.vertexCount = vertexCount;
    item.vertexSlot = vertexSlot;
    item.indices = std::move(indices);
    PopulateInputLayout(ctx, vertexSlot, item.inputLayout);
    item.vertexData = std::move(vertexData);
    Enqueue(std::move(item));
    return true;
}

static void CaptureD3D11Resources(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    ComPtrGuard<ID3D11Device> device;
    ctx->GetDevice(&device.ptr);
    if (!device.ptr) return;

    // Capture shaders
    ComPtrGuard<ID3D11VertexShader> vs;
    ctx->VSGetShader(&vs.ptr, nullptr, nullptr);
    if (vs.ptr) {
        UINT size = 0;
        if (SUCCEEDED(vs.ptr->GetPrivateData(WKPDID_D3D11ShaderBytecode, &size, nullptr)) && size > 0) {
            std::vector<uint8_t> data(size);
            if (SUCCEEDED(vs.ptr->GetPrivateData(WKPDID_D3D11ShaderBytecode, &size, data.data()))) {
                CaptureItem shaderItem;
                shaderItem.type = CaptureType::Shader;
                shaderItem.shaderBytes = std::move(data);
                shaderItem.shaderStage = ShaderStage::Vertex;
                Enqueue(std::move(shaderItem));
            }
        }
    }

    ComPtrGuard<ID3D11PixelShader> ps;
    ctx->PSGetShader(&ps.ptr, nullptr, nullptr);
    if (ps.ptr) {
        UINT size = 0;
        if (SUCCEEDED(ps.ptr->GetPrivateData(WKPDID_D3D11ShaderBytecode, &size, nullptr)) && size > 0) {
            std::vector<uint8_t> data(size);
            if (SUCCEEDED(ps.ptr->GetPrivateData(WKPDID_D3D11ShaderBytecode, &size, data.data()))) {
                CaptureItem shaderItem;
                shaderItem.type = CaptureType::Shader;
                shaderItem.shaderBytes = std::move(data);
                shaderItem.shaderStage = ShaderStage::Pixel;
                Enqueue(std::move(shaderItem));
            }
        }
    }

    // Capture texture from PS slot 0
    ComPtrGuard<ID3D11ShaderResourceView> srvs[8];
    ctx->PSGetShaderResources(0, 8, reinterpret_cast<ID3D11ShaderResourceView**>(srvs));
    for (int slot = 0; slot < 8; ++slot) {
        if (!srvs[slot].ptr) continue;
        ComPtrGuard<ID3D11Resource> res;
        srvs[slot].ptr->GetResource(&res.ptr);
        ComPtrGuard<ID3D11Texture2D> tex;
        if (res.ptr && SUCCEEDED(res.ptr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex.ptr)))) {
            D3D11_TEXTURE2D_DESC desc{};
            tex.ptr->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE;
            ComPtrGuard<ID3D11Texture2D> staging;
            if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging.ptr))) {
                ctx->CopyResource(staging.ptr, tex.ptr);
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvs[slot].ptr->GetDesc(&srvDesc);

                DXGI_FORMAT viewFormat = srvDesc.Format != DXGI_FORMAT_UNKNOWN ? srvDesc.Format : desc.Format;
                if (viewFormat == DXGI_FORMAT_UNKNOWN || IsTypelessFormat(viewFormat)) {
                    LogOnce(LogOnceId::TypelessTextureFormat, L"Skipping typeless SRV texture capture.");
                    continue;
                }

                UINT firstMip = 0;
                UINT mipLevels = desc.MipLevels;
                UINT firstSlice = 0;
                UINT arraySize = desc.ArraySize;
                bool isCube = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;

                switch (srvDesc.ViewDimension) {
                case D3D11_SRV_DIMENSION_TEXTURE2D:
                    firstMip = srvDesc.Texture2D.MostDetailedMip;
                    mipLevels = srvDesc.Texture2D.MipLevels;
                    break;
                case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                    firstMip = srvDesc.Texture2DArray.MostDetailedMip;
                    mipLevels = srvDesc.Texture2DArray.MipLevels;
                    firstSlice = srvDesc.Texture2DArray.FirstArraySlice;
                    arraySize = srvDesc.Texture2DArray.ArraySize;
                    break;
                case D3D11_SRV_DIMENSION_TEXTURECUBE:
                    firstMip = srvDesc.TextureCube.MostDetailedMip;
                    mipLevels = srvDesc.TextureCube.MipLevels;
                    firstSlice = 0;
                    arraySize = 6;
                    isCube = true;
                    break;
                case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
                    firstMip = srvDesc.TextureCubeArray.MostDetailedMip;
                    mipLevels = srvDesc.TextureCubeArray.MipLevels;
                    firstSlice = srvDesc.TextureCubeArray.First2DArrayFace;
                    arraySize = srvDesc.TextureCubeArray.NumCubes * 6;
                    isCube = true;
                    break;
                default:
                    LogOnce(LogOnceId::UnsupportedTextureView, L"Unsupported SRV texture view; skipping capture.");
                    continue;
                }

                if (mipLevels == UINT(-1)) {
                    mipLevels = desc.MipLevels > firstMip ? (desc.MipLevels - firstMip) : 1;
                }
                if (firstMip + mipLevels > desc.MipLevels) {
                    mipLevels = desc.MipLevels > firstMip ? (desc.MipLevels - firstMip) : 1;
                }
                if (firstSlice + arraySize > desc.ArraySize) {
                    arraySize = desc.ArraySize > firstSlice ? (desc.ArraySize - firstSlice) : 1;
                }

                if (desc.SampleDesc.Count > 1) {
                    LogOnce(LogOnceId::UnsupportedTextureMSAA, L"Skipping MSAA texture capture.");
                    continue;
                }

                CaptureItem texItem;
                texItem.type = CaptureType::Texture;
                texItem.texFormat = viewFormat;
                texItem.texMipLevels = mipLevels == 0 ? 1 : mipLevels;
                texItem.texArraySize = arraySize == 0 ? 1 : arraySize;
                texItem.texIsCube = isCube;
                texItem.texDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
                texItem.texWidth = std::max(1u, desc.Width >> firstMip);
                texItem.texHeight = std::max(1u, desc.Height >> firstMip);
                texItem.texRowPitch = static_cast<UINT>(CalcRowPitch(viewFormat, texItem.texWidth));

                size_t totalSize = 0;
                for (UINT slice = 0; slice < texItem.texArraySize; ++slice) {
                    for (UINT mip = 0; mip < texItem.texMipLevels; ++mip) {
                        const UINT w = std::max(1u, texItem.texWidth >> mip);
                        const UINT h = std::max(1u, texItem.texHeight >> mip);
                        totalSize += CalcSubresourceSize(viewFormat, w, h);
                    }
                }
                texItem.textureData.reserve(totalSize);

                D3D11_MAPPED_SUBRESOURCE mapped{};
                for (UINT slice = 0; slice < texItem.texArraySize; ++slice) {
                    for (UINT mip = 0; mip < texItem.texMipLevels; ++mip) {
                        const UINT subresource = D3D11CalcSubresource(firstMip + mip, firstSlice + slice, desc.MipLevels);
                        if (SUCCEEDED(ctx->Map(staging.ptr, subresource, D3D11_MAP_READ, 0, &mapped))) {
                            const UINT w = std::max(1u, texItem.texWidth >> mip);
                            const UINT h = std::max(1u, texItem.texHeight >> mip);
                            const size_t rowSize = CalcRowPitch(viewFormat, w);
                            const size_t rowCount = CalcRowCount(viewFormat, h);
                            const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
                            for (size_t row = 0; row < rowCount; ++row) {
                                texItem.textureData.insert(texItem.textureData.end(),
                                                           src + row * mapped.RowPitch,
                                                           src + row * mapped.RowPitch + rowSize);
                            }
                            ctx->Unmap(staging.ptr, subresource);
                        } else {
                            LogOnce(LogOnceId::MapStagingSRVFailed, L"Failed to map staging SRV texture.");
                        }
                    }
                }
                Enqueue(std::move(texItem));
            } else {
                LogOnce(LogOnceId::CreateStagingTextureFailed, L"Failed to create staging texture for SRV capture.");
            }
        } else {
            LogOnce(LogOnceId::SRVNotTexture2D, L"SRV slot is not a Texture2D.");
        }
    }
}

static bool CaptureTextureFromSRV(ID3D11DeviceContext* ctx, ID3D11Device* device,
                                  ID3D11ShaderResourceView* srv, TextureCapture& out) {
    if (!ctx || !device || !srv) return false;
    ComPtrGuard<ID3D11Resource> res;
    srv->GetResource(&res.ptr);
    ComPtrGuard<ID3D11Texture2D> tex;
    if (!res.ptr || FAILED(res.ptr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex.ptr)))) {
        LogOnce(LogOnceId::SRVNotTexture2D, L"SRV slot is not a Texture2D.");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    tex.ptr->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) {
        LogOnce(LogOnceId::UnsupportedTextureMSAA, L"Skipping MSAA texture for glTF capture.");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srv->GetDesc(&srvDesc);
    DXGI_FORMAT viewFormat = srvDesc.Format != DXGI_FORMAT_UNKNOWN ? srvDesc.Format : desc.Format;
    if (viewFormat == DXGI_FORMAT_UNKNOWN || IsTypelessFormat(viewFormat)) {
        LogOnce(LogOnceId::TypelessTextureFormat, L"Skipping typeless SRV texture capture.");
        return false;
    }

    UINT mip = 0;
    UINT slice = 0;
    switch (srvDesc.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        mip = srvDesc.Texture2D.MostDetailedMip;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        mip = srvDesc.Texture2DArray.MostDetailedMip;
        slice = srvDesc.Texture2DArray.FirstArraySlice;
        break;
    default:
        LogOnce(LogOnceId::UnsupportedTextureView, L"Unsupported SRV texture view for glTF capture.");
        return false;
    }

    const UINT width = std::max(1u, desc.Width >> mip);
    const UINT height = std::max(1u, desc.Height >> mip);
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = viewFormat;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtrGuard<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging.ptr))) {
        LogOnce(LogOnceId::CreateStagingTextureFailed, L"Failed to create staging texture for glTF capture.");
        return false;
    }

    const UINT srcSubresource = D3D11CalcSubresource(mip, slice, desc.MipLevels);
    ctx->CopySubresourceRegion(staging.ptr, 0, 0, 0, 0, tex.ptr, srcSubresource, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.ptr, 0, D3D11_MAP_READ, 0, &mapped))) {
        LogOnce(LogOnceId::MapStagingSRVFailed, L"Failed to map staging SRV texture for glTF capture.");
        return false;
    }

    out.width = width;
    out.height = height;
    out.rowPitch = static_cast<UINT>(CalcRowPitch(viewFormat, width));
    out.format = viewFormat;
    out.isSrgb = IsSrgbFormat(viewFormat);
    const size_t rowCount = CalcRowCount(viewFormat, height);
    out.data.resize(out.rowPitch * rowCount);
    for (size_t row = 0; row < rowCount; ++row) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        uint8_t* dst = out.data.data() + row * out.rowPitch;
        memcpy(dst, src, out.rowPitch);
    }
    ctx->Unmap(staging.ptr, 0);

    uint64_t hash = HashCombine(1469598103934665603ull, out.width);
    hash = HashCombine(hash, out.height);
    hash = HashCombine(hash, static_cast<uint64_t>(out.format));
    hash = HashBytes(out.data.data(), out.data.size(), hash);
    out.hash = hash;
    return true;
}

static int CaptureFrameTexture(ID3D11DeviceContext* ctx, ID3D11Device* device,
                               ID3D11ShaderResourceView* srv, FrameCapture& frame) {
    if (!srv) return -1;
    ComPtrGuard<ID3D11Resource> res;
    srv->GetResource(&res.ptr);
    if (!res.ptr) return -1;
    void* identity = res.ptr;
    ComPtrGuard<IUnknown> identityObj;
    if (SUCCEEDED(res.ptr->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identityObj.ptr))) && identityObj.ptr) {
        identity = identityObj.ptr;
    }

    auto itPtr = frame.texturePtrToIndex.find(identity);
    if (itPtr != frame.texturePtrToIndex.end()) {
        return static_cast<int>(itPtr->second);
    }

    TextureCapture tex;
    if (!CaptureTextureFromSRV(ctx, device, srv, tex)) {
        return -1;
    }

    if (g_gltfDedupTextures.load()) {
        auto itHash = frame.textureHashToIndex.find(tex.hash);
        if (itHash != frame.textureHashToIndex.end()) {
            frame.texturePtrToIndex[identity] = itHash->second;
            return static_cast<int>(itHash->second);
        }
    }

    const size_t index = frame.textures.size();
    frame.textures.push_back(std::move(tex));
    frame.texturePtrToIndex[identity] = index;
    frame.textureHashToIndex[frame.textures.back().hash] = index;
    return static_cast<int>(index);
}

static int CaptureFrameSampler(ID3D11SamplerState* sampler, FrameCapture& frame) {
    if (!sampler) return -1;
    D3D11_SAMPLER_DESC desc{};
    sampler->GetDesc(&desc);
    uint64_t hash = HashBytes(&desc, sizeof(desc));
    if (g_gltfDedupSamplers.load()) {
        auto it = frame.samplerHashToIndex.find(hash);
        if (it != frame.samplerHashToIndex.end()) {
            return static_cast<int>(it->second);
        }
    }

    const size_t index = frame.samplers.size();
    SamplerCapture capture;
    capture.desc = desc;
    capture.hash = hash;
    frame.samplers.push_back(capture);
    frame.samplerHashToIndex[hash] = index;
    return static_cast<int>(index);
}

static float ScoreMatrixCandidate(const float* m, bool columnMajor) {
    float maxAbs = 0.0f;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return -1.0f;
        maxAbs = std::max(maxAbs, std::fabs(m[i]));
    }
    if (maxAbs > 100000.0f) return -1.0f;

    float score = 0.0f;
    if (std::fabs(m[15] - 1.0f) < 0.01f) score += 1.0f;
    else if (std::fabs(m[15]) < 0.01f) score += 0.25f;

    const float lastRowSum = columnMajor
        ? (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]))
        : (std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]));
    if (lastRowSum < 0.01f) score += 1.0f;

    const float tSum = columnMajor
        ? (std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]))
        : (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]));
    score += std::min(tSum, 100.0f) / 100.0f;

    auto length3 = [](float x, float y, float z) {
        return std::sqrt(x * x + y * y + z * z);
    };
    float len0 = length3(m[0], m[1], m[2]);
    float len1 = length3(m[4], m[5], m[6]);
    float len2 = length3(m[8], m[9], m[10]);
    if (len0 > 0.01f) score += 1.0f - std::min(std::fabs(len0 - 1.0f), 1.0f);
    if (len1 > 0.01f) score += 1.0f - std::min(std::fabs(len1 - 1.0f), 1.0f);
    if (len2 > 0.01f) score += 1.0f - std::min(std::fabs(len2 - 1.0f), 1.0f);
    return score;
}

static bool FindLikelyTransformMatrix(const std::vector<BufferCapture>& buffers, MatrixSource& outSource, float outMatrix[16]) {
    float bestScore = -1.0f;
    MatrixSource bestSource{};
    float bestMatrix[16]{};

    for (const auto& buffer : buffers) {
        if (buffer.data.size() < sizeof(float) * 16) continue;
        const float* floats = reinterpret_cast<const float*>(buffer.data.data());
        const size_t floatCount = buffer.data.size() / sizeof(float);
        for (size_t i = 0; i + 15 < floatCount; i += 4) {
            const float* candidate = floats + i;
            float scoreCol = ScoreMatrixCandidate(candidate, true);
            if (scoreCol > bestScore) {
                bestScore = scoreCol;
                bestSource = {};
                bestSource.valid = true;
                bestSource.stage = ShaderStage::Vertex;
                bestSource.slot = buffer.slot;
                bestSource.offsetBytes = static_cast<UINT>(i * sizeof(float));
                bestSource.columnMajor = true;
                memcpy(bestMatrix, candidate, sizeof(bestMatrix));
            }
            float scoreRow = ScoreMatrixCandidate(candidate, false);
            if (scoreRow > bestScore) {
                bestScore = scoreRow;
                bestSource = {};
                bestSource.valid = true;
                bestSource.stage = ShaderStage::Vertex;
                bestSource.slot = buffer.slot;
                bestSource.offsetBytes = static_cast<UINT>(i * sizeof(float));
                bestSource.columnMajor = false;
                memcpy(bestMatrix, candidate, sizeof(bestMatrix));
            }
        }
    }

    if (bestScore <= 0.0f) return false;
    outSource = bestSource;
    memcpy(outMatrix, bestMatrix, sizeof(bestMatrix));
    return true;
}

static void CaptureD3D11FrameDraw(ID3D11DeviceContext* ctx, bool indexed,
                                  UINT indexCount, UINT startIndex, INT baseVertex,
                                  UINT vertexCount, UINT startVertex,
                                  UINT instanceCount, UINT startInstance) {
    if (!ctx) return;
    if (!g_frameCaptureActive.load() && !g_frameCapturePendingStart.load()) return;
    if (g_shutdown.load()) return;
    thread_local bool reentry = false;
    if (reentry) return;
    reentry = true;

    StartFrameCaptureIfNeeded();
    if (!g_frameCaptureActive.load()) {
        reentry = false;
        return;
    }

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        LogOnce(LogOnceId::UnsupportedTopology, L"Skipping non-trianglelist topology for capture.");
        reentry = false;
        return;
    }

    ComPtrGuard<ID3D11Device> device;
    ctx->GetDevice(&device.ptr);
    if (!device.ptr) {
        reentry = false;
        return;
    }

    DrawCapture draw;
    draw.indexed = indexed;
    draw.topology = topology;
    draw.instanceCount = instanceCount == 0 ? 1 : instanceCount;
    draw.startInstance = startInstance;
    PopulateInputLayout(ctx, 0, draw.inputLayout);
    if (draw.inputLayout.empty()) {
        LogOnce(LogOnceId::MissingInputLayout, L"No input layout metadata for frame capture.");
        reentry = false;
        return;
    }

    uint32_t minIndex = 0;
    uint32_t maxIndex = 0;
    std::vector<uint32_t> indices;
    if (indexed) {
        if (indexCount == 0) { reentry = false; return; }
        ComPtrGuard<ID3D11Buffer> ib;
        DXGI_FORMAT idxFmt = DXGI_FORMAT_UNKNOWN;
        ctx->IAGetIndexBuffer(&ib.ptr, &idxFmt, nullptr);
        if (!ib.ptr) { reentry = false; return; }
        if (idxFmt != DXGI_FORMAT_R16_UINT && idxFmt != DXGI_FORMAT_R32_UINT) {
            LogOnce(LogOnceId::UnsupportedIndexFormat, L"Unsupported index buffer format; skipping frame capture.");
            reentry = false;
            return;
        }
        const UINT indexStride = (idxFmt == DXGI_FORMAT_R32_UINT) ? 4 : 2;
        std::vector<uint8_t> indexBytes;
        if (!ReadBufferRange(ctx, device.ptr, ib.ptr,
                             static_cast<size_t>(startIndex) * indexStride,
                             static_cast<size_t>(indexCount) * indexStride,
                             indexBytes)) {
            LogOnce(LogOnceId::MapStagingIBFailed, L"Failed to read index buffer for frame capture.");
            reentry = false;
            return;
        }
        indices.resize(indexCount);
        minIndex = UINT32_MAX;
        maxIndex = 0;
        for (UINT i = 0; i < indexCount; ++i) {
            uint32_t idx = 0;
            if (indexStride == 4) idx = reinterpret_cast<const uint32_t*>(indexBytes.data())[i];
            else idx = reinterpret_cast<const uint16_t*>(indexBytes.data())[i];
            const int64_t actual = static_cast<int64_t>(idx) + static_cast<int64_t>(baseVertex);
            if (actual < 0) {
                LogOnce(LogOnceId::InvalidVertexRange, L"D3D11 base vertex produced negative index; skipping frame capture.");
                reentry = false;
                return;
            }
            const uint32_t actualIndex = static_cast<uint32_t>(actual);
            minIndex = std::min(minIndex, actualIndex);
            maxIndex = std::max(maxIndex, actualIndex);
            indices[i] = actualIndex;
        }
        for (auto& idx : indices) idx -= minIndex;
    } else {
        if (vertexCount == 0) { reentry = false; return; }
        minIndex = startVertex;
        maxIndex = startVertex + vertexCount - 1;
        indices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) {
            indices[i] = i;
        }
    }

    if (minIndex > maxIndex) {
        reentry = false;
        return;
    }

    draw.indices = std::move(indices);
    draw.minIndex = minIndex;
    draw.maxIndex = maxIndex;
    draw.vertexCount = maxIndex - minIndex + 1;

    ID3D11Buffer* vbs[kMaxFrameVertexBuffers] = {};
    UINT strides[kMaxFrameVertexBuffers] = {};
    UINT offsets[kMaxFrameVertexBuffers] = {};
    ctx->IAGetVertexBuffers(0, kMaxFrameVertexBuffers, vbs, strides, offsets);
    ComPtrGuard<ID3D11Buffer> vbGuards[kMaxFrameVertexBuffers];
    for (UINT i = 0; i < kMaxFrameVertexBuffers; ++i) vbGuards[i].ptr = vbs[i];

    std::unordered_set<UINT> vertexSlots;
    for (const auto& elem : draw.inputLayout) {
        if (elem.inputClass != D3D11_INPUT_PER_VERTEX_DATA) continue;
        if (elem.slot < kMaxFrameVertexBuffers) vertexSlots.insert(elem.slot);
    }

    for (UINT slot : vertexSlots) {
        if (!vbs[slot] || strides[slot] == 0) continue;
        std::vector<uint8_t> data;
        if (!ReadVertexStreamRange(ctx, device.ptr, vbs[slot], strides[slot], offsets[slot], minIndex, maxIndex, data)) {
            continue;
        }
        VertexStreamCapture stream;
        stream.slot = slot;
        stream.stride = strides[slot];
        stream.offset = offsets[slot];
        stream.data = std::move(data);
        draw.vertexStreams.push_back(std::move(stream));
    }

    const InputElementInfo* posElem = FindElementAnySlot(draw.inputLayout, "POSITION", 0);
    if (!posElem || !FindVertexStream(draw.vertexStreams, posElem->slot)) {
        LogOnce(LogOnceId::MissingPosition, L"No POSITION semantic/stream found for frame capture.");
        reentry = false;
        return;
    }

    std::unordered_set<UINT> instanceSlots;
    for (const auto& elem : draw.inputLayout) {
        if (elem.inputClass != D3D11_INPUT_PER_INSTANCE_DATA) continue;
        if (elem.slot < kMaxFrameVertexBuffers) instanceSlots.insert(elem.slot);
    }
    for (UINT slot : instanceSlots) {
        if (!vbs[slot] || strides[slot] == 0) continue;
        const size_t instanceBytes = static_cast<size_t>(draw.instanceCount) * strides[slot];
        if (instanceBytes == 0) continue;
        if (instanceBytes > kMaxInstanceBufferCaptureBytes) continue;
        std::vector<uint8_t> data;
        const size_t offset = static_cast<size_t>(offsets[slot]) + static_cast<size_t>(startInstance) * strides[slot];
        if (!ReadBufferRange(ctx, device.ptr, vbs[slot], offset, instanceBytes, data)) continue;
        InstanceStreamCapture inst;
        inst.slot = slot;
        inst.stride = strides[slot];
        inst.startInstance = startInstance;
        inst.instanceCount = draw.instanceCount;
        inst.data = std::move(data);
        draw.instanceStreams.push_back(std::move(inst));
    }

    ID3D11Buffer* vsCbs[kMaxFrameConstantBuffers] = {};
    ctx->VSGetConstantBuffers(0, kMaxFrameConstantBuffers, vsCbs);
    for (UINT slot = 0; slot < kMaxFrameConstantBuffers; ++slot) {
        if (!vsCbs[slot]) continue;
        ComPtrGuard<ID3D11Buffer> cb(vsCbs[slot]);
        D3D11_BUFFER_DESC desc{};
        cb->GetDesc(&desc);
        if (desc.ByteWidth == 0 || desc.ByteWidth > kMaxConstantBufferCaptureBytes) continue;
        std::vector<uint8_t> data;
        if (!ReadBufferRange(ctx, device.ptr, cb.ptr, 0, desc.ByteWidth, data)) continue;
        BufferCapture capture;
        capture.slot = slot;
        capture.data = std::move(data);
        draw.vsConstantBuffers.push_back(std::move(capture));
    }

    ID3D11Buffer* psCbs[kMaxFrameConstantBuffers] = {};
    ctx->PSGetConstantBuffers(0, kMaxFrameConstantBuffers, psCbs);
    for (UINT slot = 0; slot < kMaxFrameConstantBuffers; ++slot) {
        if (!psCbs[slot]) continue;
        ComPtrGuard<ID3D11Buffer> cb(psCbs[slot]);
        D3D11_BUFFER_DESC desc{};
        cb->GetDesc(&desc);
        if (desc.ByteWidth == 0 || desc.ByteWidth > kMaxConstantBufferCaptureBytes) continue;
        std::vector<uint8_t> data;
        if (!ReadBufferRange(ctx, device.ptr, cb.ptr, 0, desc.ByteWidth, data)) continue;
        BufferCapture capture;
        capture.slot = slot;
        capture.data = std::move(data);
        draw.psConstantBuffers.push_back(std::move(capture));
    }

    if (FindLikelyTransformMatrix(draw.vsConstantBuffers, draw.transformSource, draw.transform)) {
        draw.hasTransform = true;
    }

    ComPtrGuard<ID3D11ShaderResourceView> srvs[kMaxFrameSrvs];
    ctx->PSGetShaderResources(0, kMaxFrameSrvs, reinterpret_cast<ID3D11ShaderResourceView**>(srvs));
    ComPtrGuard<ID3D11SamplerState> samplers[kMaxFrameSamplers];
    ctx->PSGetSamplers(0, kMaxFrameSamplers, reinterpret_cast<ID3D11SamplerState**>(samplers));

    if (!g_frameCaptureActive.load()) {
        reentry = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        if (!g_frameCaptureActive.load()) {
            reentry = false;
            return;
        }
        for (UINT slot = 0; slot < kMaxFrameSrvs; ++slot) {
            if (!srvs[slot].ptr) continue;
            int idx = CaptureFrameTexture(ctx, device.ptr, srvs[slot].ptr, g_frameCapture);
            draw.srvTextures[slot] = idx;
        }
        for (UINT slot = 0; slot < kMaxFrameSamplers; ++slot) {
            if (!samplers[slot].ptr) continue;
            int idx = CaptureFrameSampler(samplers[slot].ptr, g_frameCapture);
            draw.samplerBindings[slot] = idx;
        }
        g_frameCapture.draws.push_back(std::move(draw));
    }

    reentry = false;
}

static void CaptureD3D11Indexed(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex,
                                UINT instanceCount, UINT startInstance) {
    CaptureD3D11FrameDraw(ctx, true, indexCount, startIndex, baseVertex, 0, 0, instanceCount, startInstance);
    if (!g_captureRequested.load() && !g_captureEveryFrame.load()) return;
    if (g_shutdown.load()) return;
    if (!g_captureRequested.load() && g_captureEveryFrame.load() && !AllowAutoCapture()) return;
    thread_local bool reentry = false;
    if (reentry) return;
    reentry = true;

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        LogOnce(LogOnceId::UnsupportedTopology, L"Skipping non-trianglelist topology for capture.");
        reentry = false;
        return;
    }

    ComPtrGuard<ID3D11Buffer> vbs[4];
    UINT strides[4] = {};
    UINT offsets[4] = {};
    ctx->IAGetVertexBuffers(0, 4, reinterpret_cast<ID3D11Buffer**>(vbs), strides, offsets);
    if (!vbs[0].ptr && !vbs[1].ptr && !vbs[2].ptr && !vbs[3].ptr) { reentry = false; return; }

    ID3D11Buffer* firstVB = nullptr;
    UINT firstStride = 0;
    UINT firstOffset = 0;
    UINT firstSlot = 0;
    for (UINT i = 0; i < 4; ++i) {
        if (vbs[i].ptr) { firstVB = vbs[i].ptr; firstStride = strides[i]; firstOffset = offsets[i]; firstSlot = i; break; }
    }
    if (!firstVB || firstStride == 0) { reentry = false; return; }

    ComPtrGuard<ID3D11Buffer> ib;
    DXGI_FORMAT idxFmt = DXGI_FORMAT_UNKNOWN;
    ctx->IAGetIndexBuffer(&ib.ptr, &idxFmt, nullptr);
    if (!ib.ptr || indexCount == 0) { reentry = false; return; }
    if (idxFmt != DXGI_FORMAT_R16_UINT && idxFmt != DXGI_FORMAT_R32_UINT) {
        LogOnce(LogOnceId::UnsupportedIndexFormat, L"Unsupported index buffer format; skipping capture.");
        reentry = false;
        return;
    }

    ComPtrGuard<ID3D11Device> device;
    ctx->GetDevice(&device.ptr);
    if (!device.ptr) { reentry = false; return; }

    const UINT indexStride = (idxFmt == DXGI_FORMAT_R32_UINT) ? 4 : 2;
    std::vector<uint8_t> indexBytes;
    if (!ReadBufferRange(ctx, device.ptr, ib.ptr,
                         static_cast<size_t>(startIndex) * indexStride,
                         static_cast<size_t>(indexCount) * indexStride,
                         indexBytes)) {
        LogOnce(LogOnceId::MapStagingIBFailed, L"Failed to read index buffer.");
        reentry = false;
        return;
    }

    std::vector<uint32_t> indices(indexCount);
    uint32_t minIndex = UINT32_MAX;
    uint32_t maxIndex = 0;
    for (UINT i = 0; i < indexCount; ++i) {
        uint32_t idx = 0;
        if (indexStride == 4) {
            idx = reinterpret_cast<const uint32_t*>(indexBytes.data())[i];
        } else {
            idx = reinterpret_cast<const uint16_t*>(indexBytes.data())[i];
        }
        const int64_t actual = static_cast<int64_t>(idx) + static_cast<int64_t>(baseVertex);
        if (actual < 0) {
            LogOnce(LogOnceId::InvalidVertexRange, L"D3D11 base vertex produced negative index; skipping capture.");
            reentry = false;
            return;
        }
        const uint32_t actualIndex = static_cast<uint32_t>(actual);
        minIndex = std::min(minIndex, actualIndex);
        maxIndex = std::max(maxIndex, actualIndex);
        indices[i] = actualIndex;
    }

    if (minIndex > maxIndex) {
        reentry = false;
        return;
    }

    for (auto& idx : indices) idx -= minIndex;

    CaptureD3D11Mesh(ctx, firstSlot, firstVB, firstStride, firstOffset, minIndex, maxIndex, std::move(indices));
    CaptureD3D11Resources(ctx);

    g_captureRequested.store(false);
    reentry = false;
}

static void CaptureD3D11NonIndexed(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex,
                                   UINT instanceCount, UINT startInstance) {
    CaptureD3D11FrameDraw(ctx, false, 0, 0, 0, vertexCount, startVertex, instanceCount, startInstance);
    if (!g_captureRequested.load() && !g_captureEveryFrame.load()) return;
    if (g_shutdown.load()) return;
    if (vertexCount == 0) return;
    if (!g_captureRequested.load() && g_captureEveryFrame.load() && !AllowAutoCapture()) return;
    thread_local bool reentry = false;
    if (reentry) return;
    reentry = true;

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        LogOnce(LogOnceId::UnsupportedTopology, L"Skipping non-trianglelist topology for capture.");
        reentry = false;
        return;
    }

    ComPtrGuard<ID3D11Buffer> vbs[4];
    UINT strides[4] = {};
    UINT offsets[4] = {};
    ctx->IAGetVertexBuffers(0, 4, reinterpret_cast<ID3D11Buffer**>(vbs), strides, offsets);
    if (!vbs[0].ptr && !vbs[1].ptr && !vbs[2].ptr && !vbs[3].ptr) { reentry = false; return; }

    ID3D11Buffer* firstVB = nullptr;
    UINT firstStride = 0;
    UINT firstOffset = 0;
    UINT firstSlot = 0;
    for (UINT i = 0; i < 4; ++i) {
        if (vbs[i].ptr) { firstVB = vbs[i].ptr; firstStride = strides[i]; firstOffset = offsets[i]; firstSlot = i; break; }
    }
    if (!firstVB || firstStride == 0) { reentry = false; return; }

    const uint32_t minIndex = startVertex;
    const uint32_t maxIndex = startVertex + vertexCount - 1;
    std::vector<uint32_t> indices(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        indices[i] = i;
    }

    CaptureD3D11Mesh(ctx, firstSlot, firstVB, firstStride, firstOffset, minIndex, maxIndex, std::move(indices));
    CaptureD3D11Resources(ctx);

    g_captureRequested.store(false);
    reentry = false;
}

static void CaptureBackbufferD3D11(IDXGISwapChain* swap) {
    if (!swap) return;
    ComPtrGuard<ID3D11Device> dev;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev.ptr))) || !dev.ptr) { Log(L"DX11 backbuffer: GetDevice failed"); return; }
    ComPtrGuard<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx.ptr);
    if (!ctx.ptr) { Log(L"DX11 backbuffer: GetImmediateContext failed"); return; }

    ComPtrGuard<ID3D11Texture2D> backbuffer;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer.ptr))) || !backbuffer.ptr) { Log(L"DX11 backbuffer: GetBuffer failed"); return; }

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtrGuard<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&stagingDesc, nullptr, &staging.ptr))) { Log(L"DX11 backbuffer: CreateTexture2D staging failed"); return; }

    ctx->CopyResource(staging.ptr, backbuffer.ptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.ptr, 0, D3D11_MAP_READ, 0, &mapped))) { LogOnce(LogOnceId::MapStagingSRVFailed, L"DX11 backbuffer: Map staging failed"); return; }

    CaptureItem texItem;
    texItem.type = CaptureType::Texture;
    texItem.texWidth = desc.Width;
    texItem.texHeight = desc.Height;
    texItem.texFormat = desc.Format;
    texItem.texMipLevels = 1;
    texItem.texArraySize = 1;
    texItem.texIsCube = false;
    texItem.texDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    texItem.texRowPitch = static_cast<UINT>(CalcRowPitch(desc.Format, desc.Width));
    const size_t rowCount = CalcRowCount(desc.Format, desc.Height);
    texItem.textureData.resize(texItem.texRowPitch * rowCount);
    for (size_t row = 0; row < rowCount; ++row) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        uint8_t* dst = texItem.textureData.data() + row * texItem.texRowPitch;
        memcpy(dst, src, texItem.texRowPitch);
    }
    ctx->Unmap(staging.ptr, 0);
    Enqueue(std::move(texItem));
    UpdateOverlay(L"Backbuffer captured", L"DX11");
}

static void CaptureBackbufferD3D9(IDirect3DDevice9* device) {
    if (!device) return;
    ComPtrGuard<IDirect3DSurface9> backbuffer;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer.ptr)) || !backbuffer.ptr) { Log(L"D3D9 backbuffer: GetBackBuffer failed"); return; }
    D3DSURFACE_DESC desc{};
    backbuffer->GetDesc(&desc);

    ComPtrGuard<IDirect3DSurface9> sysmem;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysmem.ptr, nullptr)) || !sysmem.ptr) { Log(L"D3D9 backbuffer: CreateOffscreenPlainSurface failed"); return; }
    if (FAILED(device->GetRenderTargetData(backbuffer.ptr, sysmem.ptr))) { Log(L"D3D9 backbuffer: GetRenderTargetData failed"); return; }

    D3DLOCKED_RECT lr{};
    if (FAILED(sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { Log(L"D3D9 backbuffer: LockRect failed"); return; }

    const DXGI_FORMAT fmt = D3D9FormatToDXGI(desc.Format);
    if (fmt == DXGI_FORMAT_UNKNOWN || IsTypelessFormat(fmt)) {
        LogOnce(LogOnceId::TypelessTextureFormat, L"D3D9 backbuffer format unsupported; skipping DDS.");
        sysmem->UnlockRect();
        return;
    }

    CaptureItem texItem;
    texItem.type = CaptureType::Texture;
    texItem.texWidth = desc.Width;
    texItem.texHeight = desc.Height;
    texItem.texFormat = fmt;
    texItem.texMipLevels = 1;
    texItem.texArraySize = 1;
    texItem.texIsCube = false;
    texItem.texDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    texItem.texRowPitch = static_cast<UINT>(CalcRowPitch(fmt, desc.Width));
    const size_t rowCount = CalcRowCount(fmt, desc.Height);
    texItem.textureData.resize(texItem.texRowPitch * rowCount);
    for (size_t row = 0; row < rowCount; ++row) {
        memcpy(texItem.textureData.data() + row * texItem.texRowPitch,
               reinterpret_cast<uint8_t*>(lr.pBits) + row * lr.Pitch,
               texItem.texRowPitch);
    }
    sysmem->UnlockRect();
    Enqueue(std::move(texItem));
    UpdateOverlay(L"Backbuffer captured", L"D3D9");
}

static void AttachD3D9DeviceHooks(IDirect3DDevice9* device) {
    if (g_d3d9Hooked || !device) return;
    void** vtbl = *reinterpret_cast<void***>(device);
    g_origDrawIndexedPrimitive9 = reinterpret_cast<DrawIndexedPrimitive9_t>(vtbl[82]);
    if (g_origDrawIndexedPrimitive9) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDrawIndexedPrimitive9, HookedDrawIndexedPrimitive9);
        DetourTransactionCommit();
        g_d3d9Hooked = true;
    }
}

static void AttachD3D11ContextHooks(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (g_d3d11Hooked || !device || !ctx) return;
    void** vtbl = *reinterpret_cast<void***>(ctx);
    g_origDrawIndexed11 = reinterpret_cast<DrawIndexed11_t>(vtbl[12]);
    g_origDraw11 = reinterpret_cast<Draw11_t>(vtbl[13]);
    g_origDrawInstanced11 = reinterpret_cast<DrawInstanced11_t>(vtbl[21]);
    g_origDrawIndexedInstanced11 = reinterpret_cast<DrawIndexedInstanced11_t>(vtbl[20]);
    // vtable indices 39/40 are DrawIndexedInstancedIndirect / DrawInstancedIndirect on ID3D11DeviceContext
    g_origDrawIndexedInstancedIndirect11 = reinterpret_cast<DrawIndexedInstancedIndirect11_t>(vtbl[39]);
    g_origDrawInstancedIndirect11 = reinterpret_cast<DrawInstancedIndirect11_t>(vtbl[40]);
    if (g_origDrawIndexed11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDrawIndexed11, HookedDrawIndexed11);
        DetourTransactionCommit();
        g_d3d11Hooked = true;
        Log(L"DrawIndexed11 detoured");
    }
    if (g_origDraw11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDraw11, HookedDraw11);
        DetourTransactionCommit();
        Log(L"Draw11 detoured");
    }
    if (g_origDrawInstanced11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDrawInstanced11, HookedDrawInstanced11);
        DetourTransactionCommit();
        Log(L"DrawInstanced11 detoured");
    }
    if (g_origDrawIndexedInstanced11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDrawIndexedInstanced11, HookedDrawIndexedInstanced11);
        DetourTransactionCommit();
        Log(L"DrawIndexedInstanced11 detoured");
    }
    if (g_origDrawInstancedIndirect11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDrawInstancedIndirect11, HookedDrawInstancedIndirect11);
        DetourTransactionCommit();
        Log(L"DrawInstancedIndirect detoured");
    }
    if (g_origDrawIndexedInstancedIndirect11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDrawIndexedInstancedIndirect11, HookedDrawIndexedInstancedIndirect11);
        DetourTransactionCommit();
        Log(L"DrawIndexedInstancedIndirect detoured");
    }
}

static void AttachD3D11DeviceHooks(ID3D11Device* device) {
    if (g_d3d11DeviceHooked || !device) return;
    void** vtbl = *reinterpret_cast<void***>(device);
    g_origCreateInputLayout = reinterpret_cast<CreateInputLayout_t>(vtbl[11]);
    g_origCreateVertexShader = reinterpret_cast<CreateVertexShader_t>(vtbl[12]);
    g_origCreatePixelShader = reinterpret_cast<CreatePixelShader_t>(vtbl[15]);

    if (!g_origCreateInputLayout && !g_origCreateVertexShader && !g_origCreatePixelShader) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_origCreateInputLayout) {
        DetourAttach(&(PVOID&)g_origCreateInputLayout, HookedCreateInputLayout);
    }
    if (g_origCreateVertexShader) {
        DetourAttach(&(PVOID&)g_origCreateVertexShader, HookedCreateVertexShader);
    }
    if (g_origCreatePixelShader) {
        DetourAttach(&(PVOID&)g_origCreatePixelShader, HookedCreatePixelShader);
    }
    if (DetourTransactionCommit() == NO_ERROR) {
        g_d3d11DeviceHooked = true;
        if (g_origCreateInputLayout) Log(L"CreateInputLayout detoured");
        if (g_origCreateVertexShader) Log(L"CreateVertexShader detoured");
        if (g_origCreatePixelShader) Log(L"CreatePixelShader detoured");
    }
}

// Hook implementations
static IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdk) {
    IDirect3D9* d3d9 = g_origDirect3DCreate9 ? g_origDirect3DCreate9(sdk) : nullptr;
    if (d3d9 && !g_d3d9CreateHooked) {
        void** vtbl = *reinterpret_cast<void***>(d3d9);
        g_origD3D9CreateDevice = reinterpret_cast<D3D9CreateDevice_t>(vtbl[16]); // CreateDevice index
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origD3D9CreateDevice, HookedD3D9CreateDevice);
        DetourTransactionCommit();
        g_d3d9CreateHooked = true;
    }
    return d3d9;
}

static HRESULT STDMETHODCALLTYPE HookedD3D9CreateDevice(IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                        HWND hFocusWindow, DWORD BehaviorFlags,
                                                        D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                        IDirect3DDevice9** ppReturnedDeviceInterface) {
    HRESULT hr = g_origD3D9CreateDevice(self, Adapter, DeviceType, hFocusWindow,
                                        BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface && !g_d3d9Hooked) {
        AttachD3D9DeviceHooks(*ppReturnedDeviceInterface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive9(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                                             INT baseVertexIndex, UINT minIndex, UINT numVertices,
                                                             UINT startIndex, UINT primCount) {
    if (type == D3DPT_TRIANGLELIST) {
        CaptureD3D9(device, baseVertexIndex, startIndex, primCount * 3);
    }
    return g_origDrawIndexedPrimitive9(device, type, baseVertexIndex, minIndex, numVertices, startIndex, primCount);
}

static HRESULT WINAPI HookedD3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                              UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                              UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                                              ID3D11DeviceContext** ppImmediateContext) {
    HRESULT hr = g_origD3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                         FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppImmediateContext && *ppImmediateContext && ppDevice && *ppDevice) {
        AttachD3D11ContextHooks(*ppDevice, *ppImmediateContext);
        AttachD3D11DeviceHooks(*ppDevice);
    }
    return hr;
}

static HRESULT WINAPI HookedD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                          UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                          UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                          D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    HRESULT hr = g_origD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                                     SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
                                                     ppImmediateContext);
    if (SUCCEEDED(hr) && ppImmediateContext && *ppImmediateContext && ppDevice && *ppDevice) {
        AttachD3D11ContextHooks(*ppDevice, *ppImmediateContext);
        AttachD3D11DeviceHooks(*ppDevice);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateInputLayout(ID3D11Device* device, const D3D11_INPUT_ELEMENT_DESC* descs,
                                                         UINT numElements, const void* shaderBytecodeWithInputSignature,
                                                         SIZE_T bytecodeLength, ID3D11InputLayout** outLayout) {
    HRESULT hr = g_origCreateInputLayout(device, descs, numElements, shaderBytecodeWithInputSignature, bytecodeLength, outLayout);
    if (SUCCEEDED(hr) && outLayout && *outLayout && descs && numElements > 0) {
        std::vector<InputElementInfo> elements;
        elements.reserve(numElements);
        std::unordered_map<UINT, UINT> slotOffsets;
        for (UINT i = 0; i < numElements; ++i) {
            const auto& d = descs[i];
            InputElementInfo info{};
            info.semantic = d.SemanticName ? d.SemanticName : "";
            info.semanticIndex = d.SemanticIndex;
            info.format = d.Format;
            info.slot = d.InputSlot;
            info.inputClass = d.InputSlotClass;
            info.stepRate = d.InstanceDataStepRate;
            UINT offset = d.AlignedByteOffset;
            auto& running = slotOffsets[info.slot];
            if (offset == D3D11_APPEND_ALIGNED_ELEMENT) {
                offset = running;
            }
            info.offset = offset;
            const UINT fmtSize = FormatSizeBytes(info.format);
            running = offset + fmtSize;
            elements.push_back(std::move(info));
        }
        void* key = *outLayout;
        ComPtrGuard<IUnknown> identity;
        if (SUCCEEDED((*outLayout)->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity.ptr))) &&
            identity.ptr) {
            key = identity.ptr;
        }
        std::lock_guard<std::mutex> lock(g_layoutMutex);
        g_layouts[key] = std::move(elements);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(ID3D11Device* device, const void* shaderBytecode,
                                                          SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                          ID3D11VertexShader** outShader) {
    HRESULT hr = g_origCreateVertexShader(device, shaderBytecode, bytecodeLength, classLinkage, outShader);
    if (SUCCEEDED(hr) && outShader && *outShader && shaderBytecode && bytecodeLength > 0) {
        (*outShader)->SetPrivateData(WKPDID_D3D11ShaderBytecode, static_cast<UINT>(bytecodeLength), shaderBytecode);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(ID3D11Device* device, const void* shaderBytecode,
                                                         SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                         ID3D11PixelShader** outShader) {
    HRESULT hr = g_origCreatePixelShader(device, shaderBytecode, bytecodeLength, classLinkage, outShader);
    if (SUCCEEDED(hr) && outShader && *outShader && shaderBytecode && bytecodeLength > 0) {
        (*outShader)->SetPrivateData(WKPDID_D3D11ShaderBytecode, static_cast<UINT>(bytecodeLength), shaderBytecode);
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedDrawIndexed11(ID3D11DeviceContext* ctx, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    CaptureD3D11Indexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation, 1, 0);
    g_origDrawIndexed11(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE HookedDraw11(ID3D11DeviceContext* ctx, UINT VertexCount, UINT StartVertexLocation) {
    CaptureD3D11NonIndexed(ctx, VertexCount, StartVertexLocation, 1, 0);
    g_origDraw11(ctx, VertexCount, StartVertexLocation);
}

static void STDMETHODCALLTYPE HookedDrawInstanced11(ID3D11DeviceContext* ctx, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    // Capture instanced draw (instance data captured for glTF).
    CaptureD3D11NonIndexed(ctx, VertexCountPerInstance, StartVertexLocation, InstanceCount, StartInstanceLocation);
    g_origDrawInstanced11(ctx, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced11(ID3D11DeviceContext* ctx, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    CaptureD3D11Indexed(ctx, IndexCountPerInstance, StartIndexLocation, BaseVertexLocation, InstanceCount, StartInstanceLocation);
    g_origDrawIndexedInstanced11(ctx, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDrawInstancedIndirect11(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT alignedOffset) {
    // Indirect buffer layout for DrawInstancedIndirect: 4 UINTs
    LogOnce(LogOnceId::IndirectInstancedSeen, L"Observed DrawInstancedIndirect.");
    struct Args { UINT VertexCountPerInstance; UINT InstanceCount; UINT StartVertex; UINT StartInstance; };
    if (args) {
        ComPtrGuard<ID3D11Device> device;
        ctx->GetDevice(&device.ptr);
        std::vector<uint8_t> data;
        if (device.ptr && ReadBufferRange(ctx, device.ptr, args, alignedOffset, sizeof(Args), data)) {
            Args a{};
            memcpy(&a, data.data(), sizeof(Args));
            CaptureD3D11NonIndexed(ctx, a.VertexCountPerInstance, a.StartVertex, a.InstanceCount, a.StartInstance);
        } else {
            LogOnce(LogOnceId::IndirectArgsFailed, L"Failed to read DrawInstancedIndirect args.");
        }
    }
    g_origDrawInstancedIndirect11(ctx, args, alignedOffset);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstancedIndirect11(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT alignedOffset) {
    // Indirect buffer layout for DrawIndexedInstancedIndirect: 5 UINTs
    LogOnce(LogOnceId::IndirectIndexedSeen, L"Observed DrawIndexedInstancedIndirect.");
    struct Args { UINT IndexCountPerInstance; UINT InstanceCount; UINT StartIndex; INT BaseVertex; UINT StartInstance; };
    if (args) {
        ComPtrGuard<ID3D11Device> device;
        ctx->GetDevice(&device.ptr);
        std::vector<uint8_t> data;
        if (device.ptr && ReadBufferRange(ctx, device.ptr, args, alignedOffset, sizeof(Args), data)) {
            Args a{};
            memcpy(&a, data.data(), sizeof(Args));
            CaptureD3D11Indexed(ctx, a.IndexCountPerInstance, a.StartIndex, a.BaseVertex, a.InstanceCount, a.StartInstance);
        } else {
            LogOnce(LogOnceId::IndirectArgsFailed, L"Failed to read DrawIndexedInstancedIndirect args.");
        }
    }
    g_origDrawIndexedInstancedIndirect11(ctx, args, alignedOffset);
}

static HRESULT STDMETHODCALLTYPE HookedPresent9(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty) {
    if (!g_d3d9Hooked) {
        AttachD3D9DeviceHooks(device);
    }
    // Hotkey fallback polling in case RegisterHotKey is blocked by the game
    if (GetAsyncKeyState(VK_F9) & 1) {
        RequestFrameCapture(L"F9 pressed (Present9)");
    }
    if (GetAsyncKeyState(VK_F10) & 1) {
        g_captureRequested.store(true);
        Log(L"F10 pressed (Present9)");
    }
    if (GetAsyncKeyState(VK_F11) & 1) {
        bool newState = !g_captureEveryFrame.load();
        SetAutoCaptureEnabled(newState);
        Log(newState ? L"F11 toggled ON (Present9)" : L"F11 toggled OFF (Present9)");
    }
    if (GetAsyncKeyState(VK_F12) & 1) {
        RequestShutdown(L"F12 shutdown requested (Present9)");
    }
    if (g_captureRequested.load() || g_captureEveryFrame.load()) {
        CaptureBackbufferD3D9(device);
        g_captureRequested.store(false);
    }
    if (g_frameCaptureActive.load()) {
        FinalizeFrameCapture(L"Frame capture completed (Present9).");
    }
    return g_origD3D9Present(device, src, dst, hwnd, dirty);
}

static void HandlePresentDXGIShared(IDXGISwapChain* swap) {
    if (!g_d3d11Hooked && swap) {
        ComPtrGuard<ID3D11Device> dev;
        if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev.ptr))) && dev.ptr) {
            ComPtrGuard<ID3D11DeviceContext> ctx;
            dev->GetImmediateContext(&ctx.ptr);
            AttachD3D11ContextHooks(dev.ptr, ctx.ptr);
            AttachD3D11DeviceHooks(dev.ptr);
        }
    }
    if (GetAsyncKeyState(VK_F9) & 1) {
        RequestFrameCapture(L"F9 pressed (DXGI Present)");
    }
    if (GetAsyncKeyState(VK_F10) & 1) {
        g_captureRequested.store(true);
        Log(L"F10 pressed (DXGI Present)");
    }
    if (GetAsyncKeyState(VK_F11) & 1) {
        bool newState = !g_captureEveryFrame.load();
        SetAutoCaptureEnabled(newState);
        Log(newState ? L"F11 toggled ON (DXGI Present)" : L"F11 toggled OFF (DXGI Present)");
    }
    if (GetAsyncKeyState(VK_F12) & 1) {
        RequestShutdown(L"F12 shutdown requested (DXGI Present)");
    }
    if (g_captureRequested.load() || g_captureEveryFrame.load()) {
        CaptureBackbufferD3D11(swap);
        g_captureRequested.store(false);
    }
    if (g_frameCaptureActive.load()) {
        FinalizeFrameCapture(L"Frame capture completed (DXGI Present).");
    }
}

static HRESULT STDMETHODCALLTYPE HookedPresentDXGI(IDXGISwapChain* swap, UINT sync, UINT flags) {
    HandlePresentDXGIShared(swap);
    return g_origDXGIPresent(swap, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HookedPresent1DXGI(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    HandlePresentDXGIShared(swap);
    if (g_origDXGIPresent1) {
        return g_origDXGIPresent1(swap, sync, flags, params);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffersDXGI(IDXGISwapChain* swap, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags) {
    HRESULT hr = g_origDXGIResizeBuffers(swap, bufferCount, width, height, newFormat, flags);
    if (SUCCEEDED(hr)) {
        // Rehook Present after resize; vtable may change
        if (swap) {
            void** vtbl = *reinterpret_cast<void***>(swap);
            auto newPresent = reinterpret_cast<PresentDXGI_t>(vtbl[8]);
            Present1DXGI_t newPresent1 = g_origDXGIPresent1;
            ComPtrGuard<IDXGISwapChain1> swap1;
            if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1.ptr))) && swap1.ptr) {
                void** vtbl1 = *reinterpret_cast<void***>(swap1.ptr);
                newPresent1 = reinterpret_cast<Present1DXGI_t>(vtbl1[22]);
            }
            if (newPresent != g_origDXGIPresent || newPresent1 != g_origDXGIPresent1) {
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                if (g_origDXGIPresent && newPresent != g_origDXGIPresent) {
                    DetourDetach(&(PVOID&)g_origDXGIPresent, HookedPresentDXGI);
                    g_origDXGIPresent = newPresent;
                    DetourAttach(&(PVOID&)g_origDXGIPresent, HookedPresentDXGI);
                }
                if (g_origDXGIPresent1 && newPresent1 != g_origDXGIPresent1) {
                    DetourDetach(&(PVOID&)g_origDXGIPresent1, HookedPresent1DXGI);
                    g_origDXGIPresent1 = newPresent1;
                    if (g_origDXGIPresent1) {
                        DetourAttach(&(PVOID&)g_origDXGIPresent1, HookedPresent1DXGI);
                    }
                }
                DetourTransactionCommit();
                Log(L"DXGI Present rehooked after ResizeBuffers");
            }
        }
    }
    return hr;
}

static HWND CreateDummyWindow() {
    static const wchar_t* kClass = L"AR_DummyWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    return CreateWindowW(kClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

static void BootstrapPresentHooks() {
    HWND hwnd = CreateDummyWindow();

    if (!g_d3d9PresentHooked) {
        HMODULE hD3D9 = LoadLibraryW(L"d3d9.dll");
        auto create = hD3D9 ? reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(hD3D9, "Direct3DCreate9")) : nullptr;
        if (create) {
            ComPtrGuard<IDirect3D9> d3d(create(D3D_SDK_VERSION));
            if (d3d.ptr) {
                D3DPRESENT_PARAMETERS pp{};
                pp.Windowed = TRUE;
                pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                pp.hDeviceWindow = hwnd ? hwnd : GetDesktopWindow();
                ComPtrGuard<IDirect3DDevice9> dev;
                if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pp.hDeviceWindow,
                                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev.ptr)) && dev.ptr) {
                    void** vtbl = *reinterpret_cast<void***>(dev.ptr);
                    g_origD3D9Present = reinterpret_cast<Present9_t>(vtbl[17]);
                    if (g_origD3D9Present) {
                        DetourTransactionBegin();
                        DetourUpdateThread(GetCurrentThread());
                        DetourAttach(&(PVOID&)g_origD3D9Present, HookedPresent9);
                        DetourTransactionCommit();
                        g_d3d9PresentHooked = true;
                        Log(L"D3D9 Present detoured");
                    }
                    else { Log(L"D3D9 Present vtbl missing"); }
                }
                else { Log(L"D3D9 dummy device creation failed"); }
            }
            else { Log(L"D3D9 create failed"); }
        }
    }

    if (!g_d3d11PresentHooked) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd ? hwnd : GetDesktopWindow();
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        ComPtrGuard<ID3D11Device> dev;
        ComPtrGuard<ID3D11DeviceContext> ctx;
        ComPtrGuard<IDXGISwapChain> swap;
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                    nullptr, 0, D3D11_SDK_VERSION, &sd, &swap.ptr,
                                                    &dev.ptr, nullptr, &ctx.ptr)) && swap.ptr) {
            void** vtbl = *reinterpret_cast<void***>(swap.ptr);
            g_origDXGIPresent = reinterpret_cast<PresentDXGI_t>(vtbl[8]);
            g_origDXGIResizeBuffers = reinterpret_cast<ResizeBuffersDXGI_t>(vtbl[13]);
            ComPtrGuard<IDXGISwapChain1> swap1;
            if (SUCCEEDED(swap.ptr->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1.ptr))) && swap1.ptr) {
                void** vtbl1 = *reinterpret_cast<void***>(swap1.ptr);
                g_origDXGIPresent1 = reinterpret_cast<Present1DXGI_t>(vtbl1[22]);
            }
            if (g_origDXGIPresent) {
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                DetourAttach(&(PVOID&)g_origDXGIPresent, HookedPresentDXGI);
                if (g_origDXGIResizeBuffers) {
                    DetourAttach(&(PVOID&)g_origDXGIResizeBuffers, HookedResizeBuffersDXGI);
                }
                if (g_origDXGIPresent1) {
                    DetourAttach(&(PVOID&)g_origDXGIPresent1, HookedPresent1DXGI);
                }
                DetourTransactionCommit();
                g_d3d11PresentHooked = true;
                Log(L"DXGI Present detoured");
                if (g_origDXGIPresent1) {
                    g_d3d11Present1Hooked = true;
                    Log(L"DXGI Present1 detoured");
                }
                g_captureRequested.store(true); // force a first capture
            }
            else { Log(L"DXGI Present vtbl missing"); }
        }
        else { Log(L"D3D11 dummy device/swapchain creation failed"); }
    }

    if (hwnd) {
        DestroyWindow(hwnd);
        UnregisterClassW(L"AR_DummyWnd", GetModuleHandleW(nullptr));
    }
}

static void TryHookD3D9() {
    if (g_d3d9FactoryHooked) return;
    HMODULE hD3D9 = GetModuleHandleW(L"d3d9.dll");
    if (!hD3D9) return;
    g_origDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(hD3D9, "Direct3DCreate9"));
    if (!g_origDirect3DCreate9) return;
    if (DetourTransactionBegin() == NO_ERROR) {
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_origDirect3DCreate9, HookedDirect3DCreate9);
        if (DetourTransactionCommit() == NO_ERROR) {
            g_d3d9FactoryHooked = true;
        }
    }
}

static void DetectGraphicsAPI() {
    if (GetModuleHandleW(L"d3d12.dll")) {
        g_api.store(GraphicsApi::DX12);
        Log(L"Detected D3D12 module loaded (unsupported by this build)");
        return;
    }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        g_api.store(GraphicsApi::Vulkan);
        Log(L"Detected Vulkan module loaded (unsupported by this build)");
        return;
    }
    if (GetModuleHandleW(L"d3d11.dll") || GetModuleHandleW(L"dxgi.dll")) {
        g_api.store(GraphicsApi::DX11);
        Log(L"Detected D3D11/DXGI modules loaded");
        return;
    }
    if (GetModuleHandleW(L"d3d9.dll")) {
        g_api.store(GraphicsApi::DX9);
        Log(L"Detected D3D9 module loaded");
        return;
    }
    g_api.store(GraphicsApi::Unknown);
    Log(L"Graphics API not detected (no D3D9/11/12 or Vulkan modules loaded)");
}

static void TryHookD3D11() {
    if (g_d3d11CreateHooked) return;
    HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
    if (!hD3D11) return;
    g_origD3D11CreateDevice = reinterpret_cast<D3D11CreateDevice_t>(GetProcAddress(hD3D11, "D3D11CreateDevice"));
    g_origD3D11CreateDeviceAndSwapChain = reinterpret_cast<D3D11CreateDeviceAndSwapChain_t>(
        GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain"));
    if (!g_origD3D11CreateDevice && !g_origD3D11CreateDeviceAndSwapChain) return;
    if (DetourTransactionBegin() == NO_ERROR) {
        DetourUpdateThread(GetCurrentThread());
        if (g_origD3D11CreateDevice) {
            DetourAttach(&(PVOID&)g_origD3D11CreateDevice, HookedD3D11CreateDevice);
        }
        if (g_origD3D11CreateDeviceAndSwapChain) {
            DetourAttach(&(PVOID&)g_origD3D11CreateDeviceAndSwapChain, HookedD3D11CreateDeviceAndSwapChain);
        }
        if (DetourTransactionCommit() == NO_ERROR) {
            g_d3d11CreateHooked = true;
        }
    }
}

// Hotkey thread toggles capture with F9/F10/F11/F12
static DWORD WINAPI HotkeyThread(LPVOID) {
    g_hotkeyThreadId = GetCurrentThreadId();
    RegisterHotKey(nullptr, 99, MOD_CONTROL | MOD_SHIFT, 'D'); // toggle debug window
    RegisterHotKey(nullptr, 4, 0, VK_F9); // capture frame (glTF)
    RegisterHotKey(nullptr, 1, 0, VK_F10); // capture once
    RegisterHotKey(nullptr, 2, 0, VK_F11); // toggle every frame
    RegisterHotKey(nullptr, 3, 0, VK_F12); // shutdown
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            switch (msg.wParam) {
            case 4: RequestFrameCapture(L"F9 pressed (Hotkey thread)"); break;
            case 1: g_captureRequested.store(true); Log(L"F10 pressed (Hotkey thread)"); break;
            case 2: {
                bool newState = !g_captureEveryFrame.load();
                SetAutoCaptureEnabled(newState);
                Log(newState ? L"F11 toggled ON (Hotkey thread)" : L"F11 toggled OFF (Hotkey thread)");
            } break;
            case 3: RequestShutdown(L"F12 shutdown requested (Hotkey thread)"); PostQuitMessage(0); break;
            case 99:
                if (g_debugWnd) {
                    BOOL visible = IsWindowVisible(g_debugWnd);
                    ShowWindow(g_debugWnd, visible ? SW_HIDE : SW_SHOW);
                    Log(visible ? L"Debug window hidden" : L"Debug window shown");
                }
                break;
            default: break;
            }
        }
        if (g_shutdown.load()) {
            PostQuitMessage(0);
        }
    }
    UnregisterHotKey(nullptr, 4);
    UnregisterHotKey(nullptr, 1);
    UnregisterHotKey(nullptr, 2);
    UnregisterHotKey(nullptr, 3);
    UnregisterHotKey(nullptr, 99);
    return 0;
}

static DWORD WINAPI CleanupThreadProc(LPVOID) {
    RemoveHooks();
    HANDLE handle = g_cleanupThreadHandle;
    g_cleanupThreadHandle = nullptr;
    if (handle) CloseHandle(handle);
    g_cleanupRequested.store(false);
    return 0;
}

static void SignalShutdownOnly() {
    g_shutdown.store(true);
    g_overlayEnabled.store(false);
    g_queueCv.notify_all();
}

static void DetachThreadsForProcessExit() {
    // Avoid std::terminate on global std::thread destructors during process exit.
    if (g_watchThread.joinable()) g_watchThread.detach();
    if (g_writerThread.joinable()) g_writerThread.detach();
}

static void RequestShutdown(const wchar_t* reason) {
    const bool wasShutdown = g_shutdown.exchange(true);
    g_overlayEnabled.store(false);
    g_queueCv.notify_all();
    if (reason && !wasShutdown) {
        Log(reason);
    }
    if (!g_cleanupRequested.exchange(true)) {
        g_cleanupThreadHandle = CreateThread(nullptr, 0, CleanupThreadProc, nullptr, 0, nullptr);
        if (!g_cleanupThreadHandle) {
            g_cleanupRequested.store(false);
        }
    }
}

static void InstallHooks() {
    DetourRestoreAfterWith();
    DetectGraphicsAPI();
    LoadConfig();
    EnsureOutputDir();
    LogSessionStart();
    g_shutdown.store(false);
    g_writerThread = std::thread(WriterThread);
    g_hotkeyThreadHandle = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, &g_hotkeyThreadId);
    g_debugThreadHandle = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // Debug window thread
        const wchar_t* clsName = L"AssetRipperDebugWnd";
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_CREATE: {
                g_debugWnd = hwnd;
                g_debugEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                              WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                              10, 10, 400, 260, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"Stop / Unhook", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              10, 280, 100, 24, hwnd, reinterpret_cast<HMENU>(1001), GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"Capture Once", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              120, 280, 100, 24, hwnd, reinterpret_cast<HMENU>(1002), GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"Capture Frame", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              230, 280, 100, 24, hwnd, reinterpret_cast<HMENU>(1004), GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"Auto Capture", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              340, 280, 100, 24, hwnd, reinterpret_cast<HMENU>(1003), GetModuleHandleW(nullptr), nullptr);
                SendMessageW(g_debugEdit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
                break;
            }
            case WM_SIZE: {
                int w = LOWORD(lParam);
                int h = HIWORD(lParam);
                if (g_debugEdit) {
                    MoveWindow(g_debugEdit, 10, 10, w - 20, h - 60, TRUE);
                }
                HWND btn = GetDlgItem(hwnd, 1001);
                if (btn) MoveWindow(btn, 10, h - 40, 100, 24, TRUE);
                btn = GetDlgItem(hwnd, 1002);
                if (btn) MoveWindow(btn, 120, h - 40, 100, 24, TRUE);
                btn = GetDlgItem(hwnd, 1004);
                if (btn) MoveWindow(btn, 230, h - 40, 100, 24, TRUE);
                btn = GetDlgItem(hwnd, 1003);
                if (btn) MoveWindow(btn, 340, h - 40, 100, 24, TRUE);
                break;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == 1001) {
                    RequestShutdown(L"Stop button clicked");
                } else if (LOWORD(wParam) == 1002) {
                    g_captureRequested.store(true);
                    Log(L"Capture once (button)");
                } else if (LOWORD(wParam) == 1004) {
                    RequestFrameCapture(L"Capture frame (button)");
                } else if (LOWORD(wParam) == 1003) {
                    bool newState = !g_captureEveryFrame.load();
                    SetAutoCaptureEnabled(newState);
                    Log(newState ? L"Toggle every draw ON (button)" : L"Toggle every draw OFF (button)");
                }
                break;
            case WM_AR_APPEND_LOG: {
                auto* payload = reinterpret_cast<std::wstring*>(lParam);
                if (g_debugEdit && payload) {
                    SendMessageW(g_debugEdit, EM_SETSEL, -1, -1);
                    SendMessageW(g_debugEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(payload->c_str()));
                    SendMessageW(g_debugEdit, EM_SCROLLCARET, 0, 0);
                }
                delete payload;
                break;
            }
            case WM_AR_STOP:
                RequestShutdown(L"Stop message received");
                PostQuitMessage(0);
                break;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                g_debugWnd = nullptr;
                g_debugEdit = nullptr;
                PostQuitMessage(0);
                break;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        RegisterClassW(&wc);
        HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW, clsName, L"AssetRipper Debug",
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 440, 360, nullptr, nullptr, wc.hInstance, nullptr);
        if (wnd) {
            ShowWindow(wnd, SW_SHOW);
            UpdateWindow(wnd);
            MSG msg{};
            bool stopPosted = false;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (g_shutdown.load() && !stopPosted) {
                    PostMessageW(wnd, WM_AR_STOP, 0, 0);
                    stopPosted = true;
                }
            }
        }
        UnregisterClassW(clsName, GetModuleHandleW(nullptr));
        return 0;
    }, nullptr, 0, nullptr);

    // Overlay thread (simple status HUD)
    g_overlayThreadHandle = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        const wchar_t* clsName = L"AssetRipperOverlayWnd";
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_CREATE:
                g_overlayWnd = hwnd;
                break;
            case WM_AR_OVERLAY:
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);
                HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
                FillRect(hdc, &rc, bg);
                DeleteObject(bg);
                SetTextColor(hdc, RGB(0, 255, 0));
                SetBkMode(hdc, TRANSPARENT);
                std::wstring l1, l2;
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    l1 = g_overlayLine1;
                    l2 = g_overlayLine2;
                }
                TextOutW(hdc, 5, 5, l1.c_str(), static_cast<int>(l1.size()));
                TextOutW(hdc, 5, 25, l2.c_str(), static_cast<int>(l2.size()));
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_DESTROY:
                g_overlayWnd = nullptr;
                PostQuitMessage(0);
                break;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        RegisterClassW(&wc);
        HWND wnd = nullptr;
        if (g_overlayEnabled.load()) {
            wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  clsName, L"", WS_POPUP, 20, 20, 420, 80,
                                  nullptr, nullptr, wc.hInstance, nullptr);
        }
        if (wnd) {
            SetLayeredWindowAttributes(wnd, 0, (BYTE)180, LWA_ALPHA);
            ShowWindow(wnd, SW_SHOW);
            UpdateWindow(wnd);
            UpdateOverlay(L"Waiting for capture...", L"");
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        UnregisterClassW(clsName, GetModuleHandleW(nullptr));
        return 0;
    }, nullptr, 0, nullptr);

    BootstrapPresentHooks();
    TryHookD3D9();
    TryHookD3D11();

    g_watchThread = std::thread([] {
        while (!g_shutdown.load()) {
            TryHookD3D9();
            TryHookD3D11();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });
}

static void RemoveHooks() {
    g_shutdown.store(true);
    g_overlayEnabled.store(false);
    g_queueCv.notify_all();
    if (g_hotkeyThreadHandle) {
        PostThreadMessage(g_hotkeyThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hotkeyThreadHandle, 1000);
        CloseHandle(g_hotkeyThreadHandle);
        g_hotkeyThreadHandle = nullptr;
    }
    if (g_watchThread.joinable()) g_watchThread.join();
    if (g_writerThread.joinable()) g_writerThread.join();

    if (g_origDrawIndexedPrimitive9) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDrawIndexedPrimitive9, HookedDrawIndexedPrimitive9);
        DetourTransactionCommit();
    }
    if (g_origD3D9Present) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origD3D9Present, HookedPresent9);
        DetourTransactionCommit();
    }
    if (g_origD3D9CreateDevice) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origD3D9CreateDevice, HookedD3D9CreateDevice);
        DetourTransactionCommit();
    }
    if (g_origDirect3DCreate9) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDirect3DCreate9, HookedDirect3DCreate9);
        DetourTransactionCommit();
    }
    if (g_origDrawIndexed11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDrawIndexed11, HookedDrawIndexed11);
        DetourTransactionCommit();
    }
    if (g_origDraw11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDraw11, HookedDraw11);
        DetourTransactionCommit();
    }
    if (g_origDrawInstanced11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDrawInstanced11, HookedDrawInstanced11);
        DetourTransactionCommit();
    }
    if (g_origDrawIndexedInstanced11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDrawIndexedInstanced11, HookedDrawIndexedInstanced11);
        DetourTransactionCommit();
    }
    if (g_origDrawInstancedIndirect11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDrawInstancedIndirect11, HookedDrawInstancedIndirect11);
        DetourTransactionCommit();
    }
    if (g_origDrawIndexedInstancedIndirect11) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDrawIndexedInstancedIndirect11, HookedDrawIndexedInstancedIndirect11);
        DetourTransactionCommit();
    }
    if (g_origDXGIPresent) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDXGIPresent, HookedPresentDXGI);
        DetourTransactionCommit();
    }
    if (g_origDXGIPresent1) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDXGIPresent1, HookedPresent1DXGI);
        DetourTransactionCommit();
    }
    if (g_origDXGIResizeBuffers) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origDXGIResizeBuffers, HookedResizeBuffersDXGI);
        DetourTransactionCommit();
    }
    if (g_origD3D11CreateDevice) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origD3D11CreateDevice, HookedD3D11CreateDevice);
        DetourTransactionCommit();
    }
    if (g_origD3D11CreateDeviceAndSwapChain) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origD3D11CreateDeviceAndSwapChain, HookedD3D11CreateDeviceAndSwapChain);
        DetourTransactionCommit();
    }
    if (g_origCreateInputLayout) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origCreateInputLayout, HookedCreateInputLayout);
        DetourTransactionCommit();
    }
    if (g_origCreateVertexShader) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origCreateVertexShader, HookedCreateVertexShader);
        DetourTransactionCommit();
    }
    if (g_origCreatePixelShader) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_origCreatePixelShader, HookedCreatePixelShader);
        DetourTransactionCommit();
    }
    {
        std::lock_guard<std::mutex> lock(g_layoutMutex);
        g_layouts.clear();
    }
    if (g_debugWnd) {
        PostMessageW(g_debugWnd, WM_CLOSE, 0, 0);
    }
    if (g_overlayWnd) {
        PostMessageW(g_overlayWnd, WM_CLOSE, 0, 0);
    }
    if (g_debugThreadHandle) {
        WaitForSingleObject(g_debugThreadHandle, 2000);
        CloseHandle(g_debugThreadHandle);
        g_debugThreadHandle = nullptr;
    }
    if (g_overlayThreadHandle) {
        WaitForSingleObject(g_overlayThreadHandle, 2000);
        CloseHandle(g_overlayThreadHandle);
        g_overlayThreadHandle = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { InstallHooks(); return 0; }, nullptr, 0, nullptr);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        SignalShutdownOnly();
        if (lpReserved) {
            DetachThreadsForProcessExit();
        }
        // Avoid heavy work under loader lock; cleanup happens on next inject or process exit.
    }
    return TRUE;
}
