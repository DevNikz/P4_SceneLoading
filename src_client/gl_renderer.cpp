#include "gl_renderer.h"
#include <glad/glad.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
// If you installed stb via vcpkg, include as below. If you provide stb_image.h in project include path, fine.
#include <stb_image.h>

static void LogGLErrorIfAny(const char* when) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        std::cerr << "[GLRenderer] GL error after " << when << ": 0x" << std::hex << err << std::dec << "\n";
    }
}

static const char* kVS = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* kFS = R"(
#version 330 core
out vec4 FragColor;
uniform vec3 uColor;
void main() {
    FragColor = vec4(uColor, 1.0);
}
)";

static const char* kSkyboxVS = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 uView;
uniform mat4 uProj;
void main() {
    TexCoords = aPos;
    vec4 pos = uProj * uView * vec4(aPos, 1.0);
    gl_Position = pos.xyww; // set depth to far plane
}
)";

static const char* kSkyboxFS = R"(
#version 330 core
in vec3 TexCoords;
out vec4 FragColor;
uniform samplerCube skybox;
void main() {
    FragColor = texture(skybox, TexCoords);
}
)";

GLRenderer::GLRenderer() = default;
GLRenderer::~GLRenderer() {
    if (program_) {
        glDeleteProgram(program_);
        std::cerr << "[GLRenderer] Deleted program " << program_ << "\n";
    }
    if (skyboxProgram_) {
        glDeleteProgram(skyboxProgram_);
        std::cerr << "[GLRenderer] Deleted skybox program " << skyboxProgram_ << "\n";
    }
    if (cubemapTex_) {
        glDeleteTextures(1, &cubemapTex_);
        std::cerr << "[GLRenderer] Deleted cubemap tex " << cubemapTex_ << "\n";
    }
    if (skyboxVBO_) {
        glDeleteBuffers(1, &skyboxVBO_);
        std::cerr << "[GLRenderer] Deleted skybox VBO " << skyboxVBO_ << "\n";
    }
    if (skyboxVAO_) {
        glDeleteVertexArrays(1, &skyboxVAO_);
        std::cerr << "[GLRenderer] Deleted skybox VAO " << skyboxVAO_ << "\n";
    }
}

void GLRenderer::Init() {
    program_ = CreateProgram(kVS, kFS);
    if (!program_) std::cerr << "[GLRenderer] Failed to create GL program\n";
    else std::cerr << "[GLRenderer] Created GL program " << program_ << "\n";

    // Create skybox program now (can be used even if no cubemap loaded)
    skyboxProgram_ = CreateProgram(kSkyboxVS, kSkyboxFS);
    if (!skyboxProgram_) std::cerr << "[GLRenderer] Failed to create skybox program\n";
    else std::cerr << "[GLRenderer] Created skybox program " << skyboxProgram_ << "\n";

    LogGLErrorIfAny("Init");
}

uint32_t GLRenderer::CompileShader(uint32_t type, const char* src) {
    uint32_t s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        std::cerr << "[GLRenderer] Shader compile error: " << buf << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

uint32_t GLRenderer::CreateProgram(const char* vs_src, const char* fs_src) {
    uint32_t vs = CompileShader(GL_VERTEX_SHADER, vs_src);
    uint32_t fs = CompileShader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    uint32_t p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    int ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        std::cerr << "[GLRenderer] Program link error: " << buf << "\n";
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (p) std::cerr << "[GLRenderer] Linked program " << p << "\n";
    LogGLErrorIfAny("CreateProgram");
    return p;
}

MeshHandle GLRenderer::UploadMesh(const std::vector<float>& vertex_positions, const std::vector<uint32_t>& indices) {
    MeshHandle h{};

    if (vertex_positions.empty() || indices.empty()) {
        std::cerr << "[GLRenderer] Warning: UploadMesh called with empty vertex or index data. verts=" 
                  << (vertex_positions.size()/3) << " indices=" << indices.size() << "\n";
    }

    glGenVertexArrays(1, &h.vao);
    glBindVertexArray(h.vao);

    glGenBuffers(1, &h.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, h.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_positions.size() * sizeof(float), vertex_positions.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &h.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, h.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    // position only (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glBindVertexArray(0);
    h.index_count = static_cast<uint32_t>(indices.size());


    LogGLErrorIfAny("UploadMesh");
    return h;
}

void GLRenderer::RenderMesh(const MeshHandle& h, const glm::mat4& model, const glm::mat4& viewProj, const glm::vec3& color) {
    // Log each render call to confirm draw invocation and primitive counts
    if (!program_ || h.vao == 0) {
        std::cerr << "[GLRenderer] RenderMesh skipped (program=" << program_ << " VAO=" << h.vao << ")\n";
        return;
    }

    glUseProgram(program_);
    GLint loc = glGetUniformLocation(program_, "uMVP");
    if (loc >= 0) {
        glm::mat4 mvp = viewProj * model;
        glUniformMatrix4fv(loc, 1, GL_FALSE, &mvp[0][0]);
    }
    GLint locc = glGetUniformLocation(program_, "uColor");
    if (locc >= 0) glUniform3fv(locc, 1, &color[0]);
    glBindVertexArray(h.vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)h.index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    LogGLErrorIfAny("RenderMesh");
}

void GLRenderer::DestroyMesh(MeshHandle& h) {
    if (h.ebo) { glDeleteBuffers(1, &h.ebo); std::cerr << "[GLRenderer] Deleted EBO " << h.ebo << "\n"; h.ebo = 0; }
    if (h.vbo) { glDeleteBuffers(1, &h.vbo); std::cerr << "[GLRenderer] Deleted VBO " << h.vbo << "\n"; h.vbo = 0; }
    if (h.vao) { glDeleteVertexArrays(1, &h.vao); std::cerr << "[GLRenderer] Deleted VAO " << h.vao << "\n"; h.vao = 0; }
    h.index_count = 0;
    LogGLErrorIfAny("DestroyMesh");
}

// Attempts to find the file by trying a series of common extensions.
static bool TryLoadImageFile(const std::filesystem::path& base, int* out_w, int* out_h, int* out_ch, unsigned char** out_data) {
    static const char* exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };
    for (auto ext : exts) {
        std::filesystem::path p = base;
        p += ext;
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
            *out_data = stbi_load(p.string().c_str(), out_w, out_h, out_ch, 0);
            if (*out_data) return true;
        }
    }
    // try as-is (maybe file already has extension in the provided name)
    if (std::filesystem::exists(base) && std::filesystem::is_regular_file(base)) {
        *out_data = stbi_load(base.string().c_str(), out_w, out_h, out_ch, 0);
        if (*out_data) return true;
    }
    return false;
}

bool GLRenderer::LoadSkybox(const std::string& folder_path) {
    namespace fs = std::filesystem;
    fs::path folder(folder_path);
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        std::cerr << "[GLRenderer] Skybox folder does not exist: " << folder.string() << "\n";
        return false;
    }


    // map to GL cubemap face order:
    // POSITIVE_X = right (rt)
    // NEGATIVE_X = left  (lf)
    // POSITIVE_Y = up    (up)
    // NEGATIVE_Y = down  (dn)
    // POSITIVE_Z = front (ft)
    // NEGATIVE_Z = back  (bk)
    std::vector<std::string> face_keys = { "rt", "lf", "up", "dn", "ft", "bk" };
    std::vector<int> widths(6), heights(6), channels(6);
    std::vector<unsigned char*> faces(6, nullptr);

    // Try base names in folder; allow files named exactly "rainbow_rt" (with extension) or "rainbow_rt.png" etc.
    // We will check for any file starting with base name if exact match not found.
    // For convenience, allow either folder/<key> or any file in folder that contains the key.
    std::string base_prefix; // detect common prefix (if any)
    // If folder contains files that include 'rt' etc, let the loop below try candidates.

    bool ok = true;
    for (int i = 0; i < 6; ++i) {
        // common candidate: folder / ("rainbow_" + key)
        // But we don't know prefix; we'll try to find a file that contains "_<key>" in name.
        unsigned char* data = nullptr;
        int w=0,h=0,ch=0;
        bool loaded = false;
        // First try exact name "rainbow_<key>" (without extension) since your file names were listed that way
        fs::path p1 = folder / ("rainbow_" + face_keys[i]);
        if (TryLoadImageFile(p1, &w, &h, &ch, &data)) {
            loaded = true;
        } else {
            // fallback: search directory for a filename that contains the key
            for (auto const& entry : fs::directory_iterator(folder)) {
                std::string fname = entry.path().filename().string();
                if (fname.find("_" + face_keys[i]) != std::string::npos) {
                    if (TryLoadImageFile(entry.path(), &w, &h, &ch, &data)) {
                        loaded = true;
                        break;
                    }
                }
            }
        }

        if (!loaded) {
            std::cerr << "[GLRenderer] Failed to load skybox face for key: " << face_keys[i] << "\n";
            ok = false;
            break;
        }

        faces[i] = data;
        widths[i] = w; heights[i] = h; channels[i] = ch;
    }

    if (!ok) {
        for (auto d : faces) if (d) stbi_image_free(d);
        return false;
    }

    // Validate sizes and channels
    for (int i = 1; i < 6; ++i) {
        if (widths[i] != widths[0] || heights[i] != heights[0]) {
            std::cerr << "[GLRenderer] Skybox faces have mismatched sizes\n";
            for (auto d : faces) if (d) stbi_image_free(d);
            return false;
        }
        if (channels[i] != channels[0]) {
            std::cerr << "[GLRenderer] Skybox faces have mismatched channel counts\n";
            for (auto d : faces) if (d) stbi_image_free(d);
            return false;
        }
    }

    // Create cubemap texture
    if (cubemapTex_) {
        glDeleteTextures(1, &cubemapTex_);
        cubemapTex_ = 0;
    }
    glGenTextures(1, &cubemapTex_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTex_);

    GLenum format = (channels[0] == 4) ? GL_RGBA : GL_RGB;
    for (GLuint i = 0; i < 6; ++i) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format, widths[i], heights[i], 0, format, GL_UNSIGNED_BYTE, faces[i]);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // free loaded images
    for (auto d : faces) if (d) stbi_image_free(d);

    // Create cube geometry VAO/VBO if not created
    if (!skyboxVAO_) {
        float skyboxVertices[] = {
            // positions
            -1.0f,  1.0f, -1.0f,
            -1.0f, -1.0f, -1.0f,
             1.0f, -1.0f, -1.0f,
             1.0f, -1.0f, -1.0f,
             1.0f,  1.0f, -1.0f,
            -1.0f,  1.0f, -1.0f,

            -1.0f, -1.0f,  1.0f,
            -1.0f, -1.0f, -1.0f,
            -1.0f,  1.0f, -1.0f,
            -1.0f,  1.0f, -1.0f,
            -1.0f,  1.0f,  1.0f,
            -1.0f, -1.0f,  1.0f,

             1.0f, -1.0f, -1.0f,
             1.0f, -1.0f,  1.0f,
             1.0f,  1.0f,  1.0f,
             1.0f,  1.0f,  1.0f,
             1.0f,  1.0f, -1.0f,
             1.0f, -1.0f, -1.0f,

            -1.0f, -1.0f,  1.0f,
            -1.0f,  1.0f,  1.0f,
             1.0f,  1.0f,  1.0f,
             1.0f,  1.0f,  1.0f,
             1.0f, -1.0f,  1.0f,
            -1.0f, -1.0f,  1.0f,

            -1.0f,  1.0f, -1.0f,
             1.0f,  1.0f, -1.0f,
             1.0f,  1.0f,  1.0f,
             1.0f,  1.0f,  1.0f,
            -1.0f,  1.0f,  1.0f,
            -1.0f,  1.0f, -1.0f,

            -1.0f, -1.0f, -1.0f,
            -1.0f, -1.0f,  1.0f,
             1.0f, -1.0f, -1.0f,
             1.0f, -1.0f, -1.0f,
            -1.0f, -1.0f,  1.0f,
             1.0f, -1.0f,  1.0f
        };
        glGenVertexArrays(1, &skyboxVAO_);
        glGenBuffers(1, &skyboxVBO_);
        glBindVertexArray(skyboxVAO_);
        glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glBindVertexArray(0);
    }

    std::cerr << "[GLRenderer] Loaded skybox cubemap tex " << cubemapTex_ << "\n";
    return true;
}

void GLRenderer::RenderSkybox(const glm::mat4& view, const glm::mat4& proj) {
    if (!cubemapTex_ || !skyboxProgram_ || !skyboxVAO_) return;

    // Save state we change
    GLboolean depthMask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLint prevDepthFunc; glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    GLint prevProg; glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);

    // Render skybox:
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    glUseProgram(skyboxProgram_);
    // remove translation from view
    glm::mat4 viewNoTrans = view;
    viewNoTrans[3] = glm::vec4(0.0f, 0.0f, 0.0f, viewNoTrans[3].w);

    GLint locView = glGetUniformLocation(skyboxProgram_, "uView");
    if (locView >= 0) glUniformMatrix4fv(locView, 1, GL_FALSE, &viewNoTrans[0][0]);
    GLint locProj = glGetUniformLocation(skyboxProgram_, "uProj");
    if (locProj >= 0) glUniformMatrix4fv(locProj, 1, GL_FALSE, &proj[0][0]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTex_);
    GLint locSampler = glGetUniformLocation(skyboxProgram_, "skybox");
    if (locSampler >= 0) glUniform1i(locSampler, 0);

    glBindVertexArray(skyboxVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    // restore state
    glDepthMask(depthMask);
    glDepthFunc(prevDepthFunc);
    glUseProgram(prevProg);

    LogGLErrorIfAny("RenderSkybox");
}