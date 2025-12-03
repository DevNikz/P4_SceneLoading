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
        auto scenes = scheduler.GetAllScenes();
        int scene_index_ui = 0;
        for (auto& sd : scenes) {
            ImGui::PushID(sd->scene_id.c_str());
            ImGui::Text("Scene: %s", sd->scene_id.c_str());
            SceneState st = sd->state.load();
            ImGui::SameLine(300);
            ImGui::TextColored((st == SceneState::LOADED) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 0, 1), "%s", (st == SceneState::LOADED) ? "LOADED" : (st == SceneState::LOADING) ? "LOADING" : (st == SceneState::QUEUED) ? "QUEUED" : "UNLOADED");
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
                scheduler.PrioritizeScene(sd->scene_id);
                // will frame below if bounds present
            }

            {
                std::scoped_lock lk(sd->mtx);
                if (!sd->model_bounds.empty()) {
                    // compute combined bounds in world space (apply same scene offset used in Render loop)
                    glm::vec3 minv(FLT_MAX), maxv(-FLT_MAX);
                    glm::vec3 offset = glm::vec3(scene_index_ui * 5.0f, 0.0f, 0.0f);
                    for (size_t m = 0; m < sd->model_bounds.size(); ++m) {
                        glm::vec3 c = sd->model_bounds[m].center + offset;
                        float r = sd->model_bounds[m].radius;
                        minv = glm::min(minv, c - glm::vec3(r));
                        maxv = glm::max(maxv, c + glm::vec3(r));
                    }
                    glm::vec3 center = (minv + maxv) * 0.5f;
                    float radius = glm::length(maxv - center);
                    camera.FrameBoundingSphere(center, radius, (float)display_w / (float)display_h);
                }
            }

            ImGui::PopID();
            ++scene_index_ui;
        }
        ImGui::End();

        // FPS display
        frame_time_avg = 0.9 * frame_time_avg + 0.1 * dt;
        fps = 1.0 / frame_time_avg;
        ImGui::Begin("Debug");
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Cam pos: (%.2f, %.2f, %.2f)", camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z);
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

        // Render loaded scenes (simple overlap: draw each model with a translate based on scene index)
        {
            int scene_index = 0;
            auto all_scenes = scheduler.GetAllScenes();
            for (auto& sd : all_scenes) {
                if (sd->state.load() != SceneState::LOADED) { ++scene_index; continue; }
                std::scoped_lock lk(sd->mtx);
                for (size_t mi = 0; mi < sd->mesh_handles.size(); ++mi) {
                    const MeshHandle& mh = sd->mesh_handles[mi];
                    if (mh.vao == 0) continue;
                    glm::mat4 model = (sd->model_transforms.size() > mi) ? sd->model_transforms[mi] : glm::mat4(1.0f);
                    // apply scene offset so overlapping scenes do not coincide exactly
                    model = glm::translate(glm::mat4(1.0f), glm::vec3(scene_index * 5.0f, 0.0f, 0.0f)) * model;
                    renderer.RenderMesh(mh, model, viewProj, glm::vec3(0.8f, 0.8f, 0.9f));
                }
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