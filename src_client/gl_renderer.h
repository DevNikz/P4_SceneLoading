#pragma once

#include <vector>
#include <cstdint>
#include <functional>
#include <mutex>
#include <glm/glm.hpp>

// Simple mesh handle
struct MeshHandle {
    uint32_t vao = 0;
    uint32_t vbo = 0;
    uint32_t ebo = 0;
    uint32_t index_count = 0;
};

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    // Call from main thread after GL context is current
    void Init();

    // Upload CPU vertex/index buffers on main thread. Returns handle.
    MeshHandle UploadMesh(const std::vector<float>& vertex_positions, const std::vector<uint32_t>& indices);

    // Render a mesh with a given model matrix and viewProj matrix and color
    void RenderMesh(const MeshHandle& h, const glm::mat4& model, const glm::mat4& viewProj, const glm::vec3& color);

    // Render a simple ground plane (large quad) beneath models. Call after Init and before rendering models.
    void RenderPlane(const glm::mat4& viewProj, const glm::vec3& color = glm::vec3(0.35f, 0.35f, 0.35f));

    // Destroy mesh resources (must be called on main thread)
    void DestroyMesh(MeshHandle& h);

    // Load a skybox cubemap from a folder containing files:
    // <base>_rt, <base>_lf, <base>_up, <base>_dn, <base>_ft, <base>_bk
    // The loader will try common extensions (.png, .jpg, .jpeg, .bmp, .tga)
    // Returns true on success.
    bool LoadSkybox(const std::string& folder_path);

    // Render the skybox. Provide view and projection matrices. view must be the camera view matrix
    // (the function removes translation so the skybox stays centered on the camera).
    void RenderSkybox(const glm::mat4& view, const glm::mat4& proj);

private:
    uint32_t CompileShader(uint32_t type, const char* src);
    uint32_t CreateProgram(const char* vs_src, const char* fs_src);

    uint32_t program_ = 0;

    // Skybox resources
    uint32_t skyboxProgram_ = 0;
    uint32_t skyboxVAO_ = 0;
    uint32_t skyboxVBO_ = 0;
    uint32_t cubemapTex_ = 0;

    // Plane resources
    uint32_t planeVAO_ = 0;
    uint32_t planeVBO_ = 0;
    uint32_t planeEBO_ = 0;
    uint32_t planeIndexCount_ = 0;
};