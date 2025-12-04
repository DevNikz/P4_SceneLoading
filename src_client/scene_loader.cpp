#include "scene_loader.h"
#include "model_loader.h"
#include "tiny_obj_loader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cfloat>
#include <glm/glm.hpp>

namespace fs = std::filesystem;

SceneLoader::SceneLoader(SceneClient* client, GLRenderer* renderer, std::queue<GLUploadTask>& upload_queue, std::mutex& upload_mtx, std::condition_variable& upload_cv, const std::string& tmp_dir, size_t worker_count)
    : client_(client), renderer_(renderer), tmp_dir_(tmp_dir), upload_queue_(upload_queue), upload_mtx_(upload_mtx), upload_cv_(upload_cv), worker_count_(worker_count) {
    fs::create_directories(tmp_dir_);
    if (worker_count_ == 0) worker_count_ = 1;
    for (size_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back(&SceneLoader::WorkerThread, this);
    }
}

SceneLoader::~SceneLoader() {
    Shutdown();
}

void SceneLoader::EnqueueLoad(const std::shared_ptr<SceneDescriptor>& scene) {
    {
        std::scoped_lock lk(queue_mtx_);
        queue_.push_back(scene);
        scene->state.store(SceneState::QUEUED);
    }
    queue_cv_.notify_one();
}

void SceneLoader::Shutdown() {
    running_ = false;
    queue_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

void SceneLoader::WorkerThread() {
    ModelLoader model_loader;
    while (running_) {
        std::shared_ptr<SceneDescriptor> scene;
        {
            std::unique_lock lk(queue_mtx_);
            queue_cv_.wait(lk, [&]() { return !queue_.empty() || !running_; });
            if (!running_) break;
            scene = queue_.front();
            queue_.pop_front();
            scene->state.store(SceneState::LOADING);
        }

        // synchronous RPC to get manifest
        scene::SceneManifest manifest;
        if (!client_->GetSceneManifest(scene->scene_id, manifest)) {
            scene->state.store(SceneState::ERROR_STATE);
            continue;
        }

        // initialize per-model containers
        {
            std::scoped_lock lk(scene->mtx);
            scene->models.clear();
            scene->mesh_handles.clear();
            scene->model_transforms.clear();
            scene->model_bounds.clear();
            scene->models.reserve(manifest.models_size());
            scene->mesh_handles.resize(manifest.models_size());
            scene->model_transforms.resize(manifest.models_size());
            scene->model_bounds.resize(manifest.models_size());
            for (int i = 0; i < manifest.models_size(); ++i) {
                const auto& mi = manifest.models(i);
                scene->models.emplace_back();
                auto& mp = scene->models.back();
                mp.name = mi.name();
                mp.rel_path = mi.rel_path();
                mp.size_bytes = mi.size_bytes();
                mp.bytes_received.store(0);
                mp.parsed = false;
                scene->model_transforms[i] = glm::mat4(1.0f);
                scene->model_bounds[i] = { glm::vec3(0.0f), 0.0f };
            }
            scene->current_model_index.store(0);
        }

        // download -> parse -> prepare GL upload (main thread)
        for (size_t i = 0; i < scene->models.size(); ++i) {
            ModelProgress& mp = scene->models[i];
            fs::path out_path = fs::path(tmp_dir_) / scene->scene_id / mp.rel_path;
            fs::create_directories(out_path.parent_path());

            auto progress_cb = [&](int64_t got, int64_t total) {
                mp.bytes_received.store(got);
            };

            if (!client_->StreamModelToFile(scene->scene_id, mp.rel_path, out_path.string(), mp.size_bytes, progress_cb)) {
                scene->state.store(SceneState::ERROR_STATE);
                break;
            }

            MeshData mesh;
            if (!model_loader.LoadOBJToMeshData(out_path.string(), mesh, 1.0f, 50)) {
                std::cerr << "ModelLoader failed: " << out_path << "\n";
                scene->state.store(SceneState::ERROR_STATE);
                break;
            }

            // compute bounding box, scale and centered model matrix (scale then translate)
            glm::vec3 minv(FLT_MAX), maxv(-FLT_MAX);
            for (size_t vi = 0; vi + 2 < mesh.positions.size(); vi += 3) {
                glm::vec3 p(mesh.positions[vi + 0], mesh.positions[vi + 1], mesh.positions[vi + 2]);
                minv = glm::min(minv, p);
                maxv = glm::max(maxv, p);
            }
            glm::vec3 center = (minv + maxv) * 0.5f;
            glm::vec3 extent = maxv - minv;
            float max_extent = std::max(std::max(extent.x, extent.y), extent.z);
            float orig_radius = 0.5f * max_extent;
            float scale = (max_extent > 0.0f) ? (1.0f / max_extent) : 1.0f;

            glm::mat4 T = glm::mat4(1.0f);
            T[3] = glm::vec4(-center, 1.0f);
            glm::mat4 S = glm::mat4(1.0f);
            S[0][0] = scale; S[1][1] = scale; S[2][2] = scale;

            // scale first, then translate: p' = scale * (p - center)
            glm::mat4 model_matrix = S * T;

            std::cerr << "[SceneLoader] Parsed " << mp.rel_path << " verts=" << (mesh.positions.size()/3)
                      << " indices=" << mesh.indices.size() << " bbox_min=(" << minv.x << "," << minv.y << "," << minv.z
                      << ") bbox_max=(" << maxv.x << "," << maxv.y << "," << maxv.z << ") orig_radius=" << orig_radius << " scale=" << scale << "\n";

            glm::vec4 tc = model_matrix * glm::vec4(center, 1.0f);
            float transformed_radius = orig_radius * scale;

            {
                std::scoped_lock lk(scene->mtx);
                if (i < scene->model_bounds.size())
                    scene->model_bounds[i] = { glm::vec3(tc), transformed_radius };
                else {
                    if (i >= scene->model_bounds.size()) scene->model_bounds.resize(i + 1);
                    scene->model_bounds[i] = { glm::vec3(tc), transformed_radius };
                }
            }

            // queue GL upload on main thread
            {
                std::vector<float> vertices = std::move(mesh.positions);
                std::vector<uint32_t> indices = std::move(mesh.indices);
                auto scene_wp = std::weak_ptr<SceneDescriptor>(scene);
                std::scoped_lock lk(upload_mtx_);
                upload_queue_.push([vertices = std::move(vertices), indices = std::move(indices), scene_wp, model_index = i, model_matrix, this]() mutable {
                    auto scene_sp = scene_wp.lock();
                    if (!scene_sp) return;
                    MeshHandle h = renderer_->UploadMesh(vertices, indices);
                    {
                        std::scoped_lock lk(scene_sp->mtx);
                        if (model_index < scene_sp->mesh_handles.size()) {
                            scene_sp->mesh_handles[model_index] = h;
                            scene_sp->model_transforms[model_index] = model_matrix;
                        }
                    }
                    std::cerr << "[SceneLoader][UploadTask] Stored MeshHandle VAO=" << h.vao << " for model_index=" << model_index << "\n";
                });
            }
            upload_cv_.notify_one();

            mp.bytes_received.store(mp.size_bytes);
            mp.parsed = true;
        }

        if (scene->state.load() != SceneState::ERROR_STATE) {
            scene->state.store(SceneState::LOADED);
        }
    }
}