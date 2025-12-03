#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include <glm/glm.hpp>
#include "gl_renderer.h"

enum class SceneState { UNLOADED, QUEUED, LOADING, LOADED, ERROR_STATE };

struct ModelProgress {
    std::string name;
    std::string rel_path;
    int64_t size_bytes = 0;
    std::atomic<int64_t> bytes_received{0};
    bool parsed = false;

    ModelProgress() = default;
    ModelProgress(const ModelProgress& o)
        : name(o.name), rel_path(o.rel_path), size_bytes(o.size_bytes),
          bytes_received(o.bytes_received.load()), parsed(o.parsed) {}
    ModelProgress& operator=(const ModelProgress& o) {
        if (this != &o) {
            name = o.name; rel_path = o.rel_path; size_bytes = o.size_bytes;
            bytes_received.store(o.bytes_received.load()); parsed = o.parsed;
        }
        return *this;
    }
    ModelProgress(ModelProgress&& o) noexcept
        : name(std::move(o.name)), rel_path(std::move(o.rel_path)), size_bytes(o.size_bytes),
          bytes_received(o.bytes_received.load()), parsed(o.parsed) {}
    ModelProgress& operator=(ModelProgress&& o) noexcept {
        if (this != &o) {
            name = std::move(o.name); rel_path = std::move(o.rel_path); size_bytes = o.size_bytes;
            bytes_received.store(o.bytes_received.load()); parsed = o.parsed;
        }
        return *this;
    }
};

struct ModelBounds {
    glm::vec3 center{0.0f};
    float radius{0.0f};
};

struct SceneDescriptor {
    std::string scene_id;
    std::vector<ModelProgress> models;
    std::atomic<SceneState> state{ SceneState::UNLOADED };
    // Simple thumbnail storage (RGBA8)
    std::vector<unsigned char> thumbnail;
    int thumb_width = 0;
    int thumb_height = 0;

    // GPU resources (one MeshHandle per model). Must be accessed with mtx.
    std::vector<MeshHandle> mesh_handles;
    // model transforms per model (local transform)
    std::vector<glm::mat4> model_transforms;

    // per-model bounds in scene-local space (center + radius)
    std::vector<ModelBounds> model_bounds;

    std::mutex mtx; // protects descriptor fields that aren't atomic
};