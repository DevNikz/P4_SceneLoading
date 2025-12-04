#include "gl_renderer.h"
#include <glad/glad.h>
#include <iostream>

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

GLRenderer::GLRenderer() = default;
GLRenderer::~GLRenderer() {
    if (program_) {
        glDeleteProgram(program_);
        std::cerr << "[GLRenderer] Deleted program " << program_ << "\n";
    }
}

void GLRenderer::Init() {
    program_ = CreateProgram(kVS, kFS);
    if (!program_) std::cerr << "[GLRenderer] Failed to create GL program\n";
    else std::cerr << "[GLRenderer] Created GL program " << program_ << "\n";
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