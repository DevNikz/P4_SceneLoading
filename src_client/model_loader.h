#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct MeshData {
    // positions: x,y,z,x,y,z,...
    std::vector<float> positions;
    // indices for triangle list
    std::vector<uint32_t> indices;
};

class ModelLoader {
public:
    ModelLoader() = default;
    ~ModelLoader() = default;

    // Synchronously load OBJ at `path` and fill MeshData (positions only).
    // Returns true on success, false on failure (reads tinyobj warnings/errors to stderr).
    // This function is thread-safe (no internal state).
    bool LoadOBJToMeshData(const std::string& path, MeshData& out, float scale = 1.0f, int artificial_ms_delay = 0) const;
};