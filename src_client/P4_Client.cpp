#include "scene_client.h"
#include "scene_loader.h"
#include "scene_scheduler.h"
#include "gl_renderer.h"
#include "scene_types.h"
#include "camera.h"

#include <grpcpp/grpcpp.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

enum class ViewMode { SHOW_NONE, SHOW_SINGLE, SHOW_ALL };

int main(int argc, char** argv) {
    // Setup gRPC channel to server
    std::string server_addr = (argc > 1) ? argv[1] : "localhost:50051";
    auto channel = grpc::CreateChannel(server_addr, grpc::InsecureChannelCredentials());
    SceneClient client(channel);

    // GL + window init
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "P4 Scene Viewer (Client)", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GL\n"; return 1;
    }

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Renderer + upload queue which main thread will execute
    GLRenderer renderer;
    renderer.Init();
    std::queue<GLUploadTask> upload_queue;
    std::mutex upload_mtx;
    std::condition_variable upload_cv;

    // Scene loader & scheduler
    SceneLoader loader(&client, &renderer, upload_queue, upload_mtx, upload_cv, "tmp");
    SceneScheduler scheduler(&loader);

    // Register a few scene ids (example)
    scheduler.RegisterScene("scene01");
    scheduler.RegisterScene("scene02");
    scheduler.RegisterScene("scene03");
    scheduler.RegisterScene("scene04");
    scheduler.RegisterScene("scene05");
    scheduler.Start();

    Camera camera;
    std::string view_scene_id;
    ViewMode view_mode = ViewMode::SHOW_NONE;
    const float scene_spacing = 2.0f; // smaller spacing when showing all

    // Timing/FPS
    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();
    double fps = 0.0;
    double frame_time_avg = 0.0;

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        // Timing (compute dt at top of loop)
        auto now = clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        // Poll events
        glfwPollEvents();

        // Update camera from input (main thread)
        camera.UpdateFromInput(window, dt);

        // Get framebuffer size early so UI can frame correctly
        int display_w = 1280, display_h = 720;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Execute pending GL upload tasks (created by loader)
        {
            std::scoped_lock lk(upload_mtx);
            while (!upload_queue.empty()) {
                auto task = std::move(upload_queue.front());
                upload_queue.pop();
                task();
            }
        }

        // Simple UI: list scenes and show progress
        ImGui::Begin("Scenes");
        // Global view controls
        if (ImGui::Button("View All")) {
            view_mode = ViewMode::SHOW_ALL;
            view_scene_id.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide Models")) {
            view_mode = ViewMode::SHOW_NONE;
            view_scene_id.clear();
        }
        ImGui::Separator();

        auto scenes = scheduler.GetAllScenes();
        int scene_index_ui = 0;
        for (auto& sd : scenes) {
            ImGui::PushID(sd->scene_id.c_str());
            ImGui::Text("Scene: %s", sd->scene_id.c_str());
            SceneState st = sd->state.load();
            ImGui::SameLine(300);
            ImGui::TextColored((st == SceneState::LOADED) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 0, 1),
                               "%s",
                               (st == SceneState::LOADED) ? "LOADED" :
                               (st == SceneState::LOADING) ? "LOADING" :
                               (st == SceneState::QUEUED) ? "QUEUED" : "UNLOADED");

            // per-scene small progress
            int64_t total_bytes = 0, got = 0;
            {
                std::scoped_lock lk(sd->mtx);
                for (auto& m : sd->models) {
                    total_bytes += m.size_bytes;
                    got += m.bytes_received.load();
                }
            }
            float pct = (total_bytes > 0) ? (float)got / (float)total_bytes : 0.0f;
            ImGui::ProgressBar(pct, ImVec2(-1, 0));

            if (ImGui::Button("Load")) {
                if (sd->state.load() == SceneState::UNLOADED) {
                    loader.EnqueueLoad(sd);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload")) {
                scheduler.UnloadScene(sd->scene_id);
                // free GL resources
                std::scoped_lock lk(sd->mtx);
                for (auto &mh : sd->mesh_handles) {
                    renderer.DestroyMesh(mh);
                }
                sd->mesh_handles.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("View")) {
                // prioritize and set single view to this scene
                scheduler.PrioritizeScene(sd->scene_id);
                view_mode = ViewMode::SHOW_SINGLE;
                view_scene_id = sd->scene_id;

                // compute base_offset (location where hidden models spawn) and frame camera there
                auto all_tmp = scheduler.GetAllScenes();
                int base_index = 0;
                for (int bi = 0; bi < (int)all_tmp.size(); ++bi) {
                    if (all_tmp[bi]->scene_id == "scene05") { base_index = bi; break; }
                }
                glm::vec3 base_offset = glm::vec3(base_index * scene_spacing, 0.0f, 0.0f);

                // frame camera to base_offset using the scene bounds radius if available
                std::scoped_lock lk(sd->mtx);
                if (!sd->model_bounds.empty()) {
                    // use the active model's radius if present
                    int active = sd->current_model_index.load();
                    if (active < 0) active = 0;
                    if (active >= (int)sd->model_bounds.size()) active = (int)sd->model_bounds.size() - 1;
                    float radius = sd->model_bounds[active].radius;
                    // ensure minimal radius
                    radius = glm::max(radius, 0.5f);
                    camera.FrameBoundingSphere(base_offset, radius, (float)display_w / (float)display_h);
                } else {
                    // fallback: just set camera target to base_offset
                    camera.SetTarget(base_offset);
                }
            }

            // Model selector (Prev/Next) for scene
            if (sd->state.load() == SceneState::LOADED) {
                std::scoped_lock lk(sd->mtx);
                int model_count = static_cast<int>(sd->models.size());
                if (model_count > 0) {
                    int idx = sd->current_model_index.load();
                    if (idx < 0) idx = 0;
                    if (idx >= model_count) idx = model_count - 1;
                    if (ImGui::Button("Prev")) {
                        idx = (idx - 1 + model_count) % model_count;
                        sd->current_model_index.store(idx);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Next")) {
                        idx = (idx + 1) % model_count;
                        sd->current_model_index.store(idx);
                    }
                    ImGui::SameLine();
                    const std::string& name = sd->models[idx].name.empty() ? sd->models[idx].rel_path : sd->models[idx].name;
                    ImGui::Text("%d/%d: %s", idx + 1, model_count, name.c_str());
                } else {
                    ImGui::Text("No models");
                }
            }

            ImGui::PopID();
            ++scene_index_ui;
        }
        ImGui::End();

        // FPS display
        frame_time_avg = 0.9 * frame_time_avg + 0.1 * dt;
        fps = (frame_time_avg > 0.0) ? 1.0 / frame_time_avg : 0.0;
        ImGui::Begin("Debug");
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Cam pos: (%.2f, %.2f, %.2f)", camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z);
        ImGui::Text("View mode: %s", (view_mode == ViewMode::SHOW_ALL) ? "All" : (view_mode == ViewMode::SHOW_SINGLE) ? view_scene_id.c_str() : "None");
        ImGui::End();

        // Render
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        // Build view/projection from camera
        auto view = camera.GetViewMatrix();
        auto proj = camera.GetProjectionMatrix((float)display_w / (float)display_h);
        glm::mat4 viewProj = proj * view;

        // Determine base offset index for scene05 (all hidden models spawn here)
        int base_index = 4; // default to scene05 (registration order)
        {
            auto all_scenes = scheduler.GetAllScenes();
            for (int i = 0; i < (int)all_scenes.size(); ++i) {
                if (all_scenes[i]->scene_id == "scene05") { base_index = i; break; }
            }
        }
        glm::vec3 base_offset = glm::vec3(base_index * scene_spacing, 0.0f, 0.0f);

        // Render logic:
        // - SHOW_NONE: render nothing (models are loaded & uploaded but hidden)
        // - SHOW_SINGLE: render only selected scene's active model at base_offset (same place as scene05)
        // - SHOW_ALL: render every loaded scene's active model at its own small offset (scene_index * spacing)
        {
            int scene_index = 0;
            auto all_scenes = scheduler.GetAllScenes();
            for (auto& sd : all_scenes) {
                if (sd->state.load() != SceneState::LOADED) { ++scene_index; continue; }

                // decide whether to render this scene
                if (view_mode == ViewMode::SHOW_NONE) { ++scene_index; continue; }
                if (view_mode == ViewMode::SHOW_SINGLE && sd->scene_id != view_scene_id) { ++scene_index; continue; }

                std::scoped_lock lk(sd->mtx);
                int active = sd->current_model_index.load();
                if (active < 0) active = 0;
                if (active >= (int)sd->mesh_handles.size()) { ++scene_index; continue; }

                const MeshHandle& mh = sd->mesh_handles[active];
                if (mh.vao == 0) { ++scene_index; continue; }

                glm::mat4 model = (sd->model_transforms.size() > (size_t)active) ? sd->model_transforms[active] : glm::mat4(1.0f);

                // choose offset:
                // - for SHOW_ALL: small per-scene offset so models are side-by-side
                // - for SHOW_SINGLE: place every visible model at base_offset (spawn location for hidden models)
                glm::vec3 offset;
                if (view_mode == ViewMode::SHOW_ALL) {
                    offset = glm::vec3(scene_index * scene_spacing, 0.0f, 0.0f);
                } else {
                    // SHOW_SINGLE: all shown models (only one) appear at base_offset
                    offset = base_offset;
                }
                model = glm::translate(glm::mat4(1.0f), offset) * model;
                //This just nudges a specific model I couldn't get centered properly lol
                float z_nudge = 0.0f;
                if (sd->scene_id == "scene02") {

                    z_nudge = -500.0f; 
                }

                offset += glm::vec3(0.0f, 0.0f, z_nudge);
                // Force vertical alignment: ensure model's world Y equals offset.y (prevents models with different internal Y from sitting far down)
                model[3].y = offset.y;

                renderer.RenderMesh(mh, model, viewProj, glm::vec3(0.8f, 0.8f, 0.9f));
                ++scene_index;
            }
        }

        // Render ImGui on top
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Shutdown
    scheduler.Stop();
    loader.Shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}