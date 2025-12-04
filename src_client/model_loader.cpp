#include "model_loader.h"
#include <tiny_obj_loader.h>
#include <iostream>
#include <chrono>
#include <thread>

bool ModelLoader::LoadOBJToMeshData(const std::string& path, MeshData& out, float scale, int artificial_ms_delay) const {
    tinyobj::ObjReaderConfig cfg;
    cfg.mtl_search_path = ""; 
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg)) {
        std::cerr << "ModelLoader: tinyobj parse failed: " << path << "\n";
        if (!reader.Warning().empty()) std::cerr << "Warning: " << reader.Warning() << "\n";
        if (!reader.Error().empty()) std::cerr << "Error: " << reader.Error() << "\n";
        return false;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    out.positions.clear();
    out.indices.clear();
    out.positions.reserve(attrib.vertices.size());

    for (const auto& shape : shapes) {
        for (const auto& idx : shape.mesh.indices) {
            int vi = idx.vertex_index;
            if (vi < 0) continue;
            out.positions.push_back(attrib.vertices[vi * 3 + 0] * scale);
            out.positions.push_back(attrib.vertices[vi * 3 + 1] * scale);
            out.positions.push_back(attrib.vertices[vi * 3 + 2] * scale);
            out.indices.push_back(static_cast<uint32_t>(out.indices.size()));
        }
    }

    // optional artificial delay to ensure non-instant loads for UI/progress testing
    if (artificial_ms_delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(artificial_ms_delay));
    }

    return true;
}