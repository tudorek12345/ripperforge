#include "core/AssetIO.h"

#include <Windows.h>
#include <wincodec.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#pragma comment(lib, "windowscodecs.lib")

namespace rf::core {

namespace {

constexpr uint32_t kDdsMagic = 0x20534444; // DDS
constexpr uint32_t kDdpfFourCc = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdpfAlphaPixels = 0x1;

constexpr uint32_t MakeFourCc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

struct DdsPixelFormat {
    uint32_t size = 0;
    uint32_t flags = 0;
    uint32_t fourCc = 0;
    uint32_t rgbBitCount = 0;
    uint32_t rMask = 0;
    uint32_t gMask = 0;
    uint32_t bMask = 0;
    uint32_t aMask = 0;
};

struct DdsHeader {
    uint32_t size = 0;
    uint32_t flags = 0;
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t pitchOrLinearSize = 0;
    uint32_t depth = 0;
    uint32_t mipMapCount = 0;
    uint32_t reserved1[11]{};
    DdsPixelFormat ddspf{};
    uint32_t caps = 0;
    uint32_t caps2 = 0;
    uint32_t caps3 = 0;
    uint32_t caps4 = 0;
    uint32_t reserved2 = 0;
};

struct DdsHeaderDx10 {
    uint32_t dxgiFormat = 0;
    uint32_t resourceDimension = 0;
    uint32_t miscFlag = 0;
    uint32_t arraySize = 0;
    uint32_t miscFlags2 = 0;
};

enum class PixelFormat {
    Unknown,
    RGBA8,
    BGRA8,
};

PixelFormat DetectDdsFormat(const DdsHeader& header, const DdsHeaderDx10* dx10) {
    if ((header.ddspf.flags & kDdpfFourCc) != 0 && header.ddspf.fourCc == MakeFourCc('D', 'X', '1', '0') &&
        dx10 != nullptr) {
        if (dx10->dxgiFormat == 28) { // DXGI_FORMAT_R8G8B8A8_UNORM
            return PixelFormat::RGBA8;
        }
        if (dx10->dxgiFormat == 87) { // DXGI_FORMAT_B8G8R8A8_UNORM
            return PixelFormat::BGRA8;
        }
        return PixelFormat::Unknown;
    }

    if ((header.ddspf.flags & kDdpfRgb) != 0 && header.ddspf.rgbBitCount == 32) {
        if (header.ddspf.rMask == 0x000000ff && header.ddspf.gMask == 0x0000ff00 &&
            header.ddspf.bMask == 0x00ff0000 && header.ddspf.aMask == 0xff000000) {
            return PixelFormat::RGBA8;
        }
        if (header.ddspf.rMask == 0x00ff0000 && header.ddspf.gMask == 0x0000ff00 &&
            header.ddspf.bMask == 0x000000ff && header.ddspf.aMask == 0xff000000) {
            return PixelFormat::BGRA8;
        }
    }

    return PixelFormat::Unknown;
}

bool LoadTextureFromDds(const std::wstring& path, TextureData& outTexture, std::string& error) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream.good()) {
        error = "Could not open DDS file.";
        return false;
    }

    uint32_t magic = 0;
    stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != kDdsMagic) {
        error = "Invalid DDS magic.";
        return false;
    }

    DdsHeader header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream.good() || header.size != 124) {
        error = "Invalid DDS header.";
        return false;
    }

    DdsHeaderDx10 dx10{};
    DdsHeaderDx10* dx10Ptr = nullptr;
    if ((header.ddspf.flags & kDdpfFourCc) != 0 && header.ddspf.fourCc == MakeFourCc('D', 'X', '1', '0')) {
        stream.read(reinterpret_cast<char*>(&dx10), sizeof(dx10));
        if (!stream.good()) {
            error = "Invalid DDS DX10 header.";
            return false;
        }
        dx10Ptr = &dx10;
    }

    const PixelFormat pixelFormat = DetectDdsFormat(header, dx10Ptr);
    if (pixelFormat == PixelFormat::Unknown) {
        error = "DDS format not supported yet (supports RGBA8/BGRA8).";
        return false;
    }

    if (header.width == 0 || header.height == 0) {
        error = "DDS has invalid dimensions.";
        return false;
    }

    const uint64_t pixelCount = static_cast<uint64_t>(header.width) * static_cast<uint64_t>(header.height);
    if (pixelCount > (static_cast<uint64_t>(1) << 30)) {
        error = "DDS texture is too large.";
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(header.width) * 4;
    std::vector<uint8_t> raw(static_cast<size_t>(header.height) * rowBytes);
    stream.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!stream.good()) {
        error = "DDS pixel payload is truncated.";
        return false;
    }

    if (pixelFormat == PixelFormat::BGRA8) {
        for (size_t i = 0; i + 3 < raw.size(); i += 4) {
            std::swap(raw[i], raw[i + 2]);
        }
    }

    outTexture.width = header.width;
    outTexture.height = header.height;
    outTexture.rgba8 = std::move(raw);
    outTexture.sourcePath = path;
    return true;
}

bool ParseObjIndex(const std::string& token, int& outV, int& outT, int& outN) {
    outV = 0;
    outT = 0;
    outN = 0;

    const size_t firstSlash = token.find('/');
    if (firstSlash == std::string::npos) {
        outV = std::stoi(token);
        return true;
    }

    const size_t secondSlash = token.find('/', firstSlash + 1);
    const std::string v = token.substr(0, firstSlash);
    const std::string t = secondSlash == std::string::npos
        ? token.substr(firstSlash + 1)
        : token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
    const std::string n = secondSlash == std::string::npos ? std::string{} : token.substr(secondSlash + 1);

    if (!v.empty()) {
        outV = std::stoi(v);
    }
    if (!t.empty()) {
        outT = std::stoi(t);
    }
    if (!n.empty()) {
        outN = std::stoi(n);
    }
    return outV != 0;
}

int ResolveObjIndex(int index, size_t count) {
    if (index > 0) {
        return index - 1;
    }
    if (index < 0) {
        return static_cast<int>(count) + index;
    }
    return -1;
}

} // namespace

bool LoadTextureFromFile(const std::wstring& path, TextureData& outTexture, std::string& error) {
    const std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::wstring lowerExt = ext;
    for (auto& c : lowerExt) {
        c = static_cast<wchar_t>(towlower(c));
    }

    if (lowerExt == L".dds") {
        return LoadTextureFromDds(path, outTexture, error);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(hr);

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || factory == nullptr) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not create WIC factory.";
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || decoder == nullptr) {
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "WIC could not decode image file.";
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || frame == nullptr) {
        decoder->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "WIC failed to get image frame.";
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || converter == nullptr) {
        frame->Release();
        decoder->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "WIC failed to create format converter.";
        return false;
    }

    hr = converter->Initialize(
        frame,
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "WIC failed to convert image to RGBA.";
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    converter->GetSize(&width, &height);
    if (width == 0 || height == 0) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Image has invalid dimensions.";
        return false;
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data());
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "WIC failed to copy image pixels.";
        return false;
    }

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    if (shouldUninitialize) {
        CoUninitialize();
    }

    outTexture.width = width;
    outTexture.height = height;
    outTexture.rgba8 = std::move(rgba);
    outTexture.sourcePath = path;
    return true;
}

bool SaveTextureToPng(const std::wstring& path, const TextureData& texture, std::string& error) {
    if (texture.width == 0 || texture.height == 0 || texture.rgba8.empty()) {
        error = "No texture loaded for export.";
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(hr);

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || factory == nullptr) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not create WIC factory.";
        return false;
    }

    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || stream == nullptr) {
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not create WIC stream.";
        return false;
    }

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        stream->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not open PNG output path.";
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || encoder == nullptr) {
        stream->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not create PNG encoder.";
        return false;
    }

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        encoder->Release();
        stream->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not initialize PNG encoder.";
        return false;
    }

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || frame == nullptr) {
        if (props != nullptr) {
            props->Release();
        }
        encoder->Release();
        stream->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not create PNG frame.";
        return false;
    }

    hr = frame->Initialize(props);
    if (FAILED(hr)) {
        if (props != nullptr) {
            props->Release();
        }
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not initialize PNG frame.";
        return false;
    }

    if (props != nullptr) {
        props->Release();
    }

    frame->SetSize(texture.width, texture.height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    frame->SetPixelFormat(&format);

    hr = frame->WritePixels(texture.height, texture.width * 4, static_cast<UINT>(texture.rgba8.size()), const_cast<BYTE*>(texture.rgba8.data()));
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = "Could not write PNG pixels.";
        return false;
    }

    frame->Commit();
    encoder->Commit();

    frame->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
    if (shouldUninitialize) {
        CoUninitialize();
    }

    return true;
}

bool LoadMeshFromObj(const std::wstring& path, MeshData& outMesh, std::string& error) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream.good()) {
        error = "Could not open OBJ file.";
        return false;
    }

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> texcoords;
    std::unordered_map<std::string, uint32_t> vertexCache;

    MeshData mesh;

    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() < 2) {
            continue;
        }

        if (line.rfind("v ", 0) == 0) {
            std::istringstream ss(line.substr(2));
            std::array<float, 3> p{};
            ss >> p[0] >> p[1] >> p[2];
            positions.push_back(p);
            continue;
        }

        if (line.rfind("vn ", 0) == 0) {
            std::istringstream ss(line.substr(3));
            std::array<float, 3> n{};
            ss >> n[0] >> n[1] >> n[2];
            normals.push_back(n);
            continue;
        }

        if (line.rfind("vt ", 0) == 0) {
            std::istringstream ss(line.substr(3));
            std::array<float, 2> t{};
            ss >> t[0] >> t[1];
            texcoords.push_back(t);
            continue;
        }

        if (line.rfind("f ", 0) == 0) {
            std::istringstream ss(line.substr(2));
            std::vector<uint32_t> faceIndices;
            std::string token;
            while (ss >> token) {
                int vi = 0;
                int ti = 0;
                int ni = 0;
                if (!ParseObjIndex(token, vi, ti, ni)) {
                    continue;
                }

                auto found = vertexCache.find(token);
                if (found != vertexCache.end()) {
                    faceIndices.push_back(found->second);
                    continue;
                }

                MeshVertex vertex{};
                const int posIndex = ResolveObjIndex(vi, positions.size());
                if (posIndex < 0 || static_cast<size_t>(posIndex) >= positions.size()) {
                    continue;
                }

                vertex.px = positions[static_cast<size_t>(posIndex)][0];
                vertex.py = positions[static_cast<size_t>(posIndex)][1];
                vertex.pz = positions[static_cast<size_t>(posIndex)][2];

                const int uvIndex = ResolveObjIndex(ti, texcoords.size());
                if (uvIndex >= 0 && static_cast<size_t>(uvIndex) < texcoords.size()) {
                    vertex.u = texcoords[static_cast<size_t>(uvIndex)][0];
                    vertex.v = texcoords[static_cast<size_t>(uvIndex)][1];
                }

                const int normalIndex = ResolveObjIndex(ni, normals.size());
                if (normalIndex >= 0 && static_cast<size_t>(normalIndex) < normals.size()) {
                    vertex.nx = normals[static_cast<size_t>(normalIndex)][0];
                    vertex.ny = normals[static_cast<size_t>(normalIndex)][1];
                    vertex.nz = normals[static_cast<size_t>(normalIndex)][2];
                }

                const uint32_t newIndex = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                vertexCache[token] = newIndex;
                faceIndices.push_back(newIndex);
            }

            if (faceIndices.size() >= 3) {
                for (size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                    mesh.indices.push_back(faceIndices[0]);
                    mesh.indices.push_back(faceIndices[i]);
                    mesh.indices.push_back(faceIndices[i + 1]);
                }
            }
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "OBJ did not contain triangle data.";
        return false;
    }

    mesh.sourcePath = path;
    outMesh = std::move(mesh);
    return true;
}

bool SaveMeshToObj(const std::wstring& path, const MeshData& mesh, std::string& error) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "No mesh loaded for OBJ export.";
        return false;
    }

    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
        error = "Could not create OBJ file.";
        return false;
    }

    stream << "# RipperForge OBJ export\n";

    for (const auto& v : mesh.vertices) {
        stream << "v " << v.px << " " << v.py << " " << v.pz << "\n";
    }
    for (const auto& v : mesh.vertices) {
        stream << "vt " << v.u << " " << v.v << "\n";
    }
    for (const auto& v : mesh.vertices) {
        stream << "vn " << v.nx << " " << v.ny << " " << v.nz << "\n";
    }

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i] + 1;
        const uint32_t b = mesh.indices[i + 1] + 1;
        const uint32_t c = mesh.indices[i + 2] + 1;
        stream << "f " << a << "/" << a << "/" << a
               << " " << b << "/" << b << "/" << b
               << " " << c << "/" << c << "/" << c << "\n";
    }

    return true;
}

bool SaveMeshToFbxAscii(const std::wstring& path, const MeshData& mesh, std::string& error) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "No mesh loaded for FBX export.";
        return false;
    }

    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
        error = "Could not create FBX file.";
        return false;
    }

    stream << "; FBX 7.4.0 generated by RipperForge\n";
    stream << "FBXHeaderExtension:  {\n";
    stream << "  FBXHeaderVersion: 1003\n";
    stream << "  FBXVersion: 7400\n";
    stream << "}\n";
    stream << "Objects: {\n";
    stream << "  Geometry: 1000, \"Geometry::RipperMesh\", \"Mesh\" {\n";
    stream << "    Vertices: *" << (mesh.vertices.size() * 3) << " {\n";
    stream << "      a: ";
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& v = mesh.vertices[i];
        stream << v.px << "," << v.py << "," << v.pz;
        if (i + 1 < mesh.vertices.size()) {
            stream << ",";
        }
    }
    stream << "\n";
    stream << "    }\n";
    stream << "    PolygonVertexIndex: *" << mesh.indices.size() << " {\n";
    stream << "      a: ";
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const int a = static_cast<int>(mesh.indices[i]);
        const int b = static_cast<int>(mesh.indices[i + 1]);
        const int c = -static_cast<int>(mesh.indices[i + 2]) - 1;
        stream << a << "," << b << "," << c;
        if (i + 3 < mesh.indices.size()) {
            stream << ",";
        }
    }
    stream << "\n";
    stream << "    }\n";
    stream << "  }\n";
    stream << "}\n";
    stream << "Connections: {\n";
    stream << "}\n";

    return true;
}

TextureData BuildCheckerTexture(uint32_t width, uint32_t height, uint32_t tileSize) {
    TextureData texture;
    texture.width = width;
    texture.height = height;
    texture.rgba8.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const bool odd = ((x / tileSize) + (y / tileSize)) % 2 == 1;
            const uint8_t value = odd ? 215 : 80;
            const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            texture.rgba8[index + 0] = value;
            texture.rgba8[index + 1] = value;
            texture.rgba8[index + 2] = value;
            texture.rgba8[index + 3] = 255;
        }
    }

    return texture;
}

MeshData BuildUnitCubeMesh() {
    MeshData mesh;

    const std::array<MeshVertex, 24> vertices = {
        MeshVertex{-1, -1, -1, 0, 0, -1, 0, 1},
        MeshVertex{1, -1, -1, 0, 0, -1, 1, 1},
        MeshVertex{1, 1, -1, 0, 0, -1, 1, 0},
        MeshVertex{-1, 1, -1, 0, 0, -1, 0, 0},
        MeshVertex{-1, -1, 1, 0, 0, 1, 0, 1},
        MeshVertex{1, -1, 1, 0, 0, 1, 1, 1},
        MeshVertex{1, 1, 1, 0, 0, 1, 1, 0},
        MeshVertex{-1, 1, 1, 0, 0, 1, 0, 0},
        MeshVertex{-1, -1, -1, -1, 0, 0, 0, 1},
        MeshVertex{-1, 1, -1, -1, 0, 0, 1, 1},
        MeshVertex{-1, 1, 1, -1, 0, 0, 1, 0},
        MeshVertex{-1, -1, 1, -1, 0, 0, 0, 0},
        MeshVertex{1, -1, -1, 1, 0, 0, 0, 1},
        MeshVertex{1, 1, -1, 1, 0, 0, 1, 1},
        MeshVertex{1, 1, 1, 1, 0, 0, 1, 0},
        MeshVertex{1, -1, 1, 1, 0, 0, 0, 0},
        MeshVertex{-1, 1, -1, 0, 1, 0, 0, 1},
        MeshVertex{1, 1, -1, 0, 1, 0, 1, 1},
        MeshVertex{1, 1, 1, 0, 1, 0, 1, 0},
        MeshVertex{-1, 1, 1, 0, 1, 0, 0, 0},
        MeshVertex{-1, -1, -1, 0, -1, 0, 0, 1},
        MeshVertex{1, -1, -1, 0, -1, 0, 1, 1},
        MeshVertex{1, -1, 1, 0, -1, 0, 1, 0},
        MeshVertex{-1, -1, 1, 0, -1, 0, 0, 0},
    };

    const std::array<uint32_t, 36> indices = {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        8, 9, 10, 8, 10, 11,
        12, 14, 13, 12, 15, 14,
        16, 17, 18, 16, 18, 19,
        20, 22, 21, 20, 23, 22,
    };

    mesh.vertices.assign(vertices.begin(), vertices.end());
    mesh.indices.assign(indices.begin(), indices.end());
    return mesh;
}

} // namespace rf::core
