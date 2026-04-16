#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rf::core {

struct TextureData {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba8;
    std::wstring sourcePath;
};

struct MeshVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::wstring sourcePath;
};

bool LoadTextureFromFile(const std::wstring& path, TextureData& outTexture, std::string& error);
bool SaveTextureToPng(const std::wstring& path, const TextureData& texture, std::string& error);

bool LoadMeshFromObj(const std::wstring& path, MeshData& outMesh, std::string& error);
bool SaveMeshToObj(const std::wstring& path, const MeshData& mesh, std::string& error);
bool SaveMeshToFbxAscii(const std::wstring& path, const MeshData& mesh, std::string& error);

TextureData BuildCheckerTexture(uint32_t width = 256, uint32_t height = 256, uint32_t tileSize = 16);
MeshData BuildUnitCubeMesh();

} // namespace rf::core
