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

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <random>
#include <functional>
#include <unordered_map>

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
    // Attempt to load skybox from the runtime's "Skybox" folder (the application runtime dir is out/build/x64-debug)
    if (!renderer.LoadSkybox("Skybox")) {
        std::cerr << "[Main] Skybox load failed or not present (expected folder: out/build/x64-debug/Skybox)\n";
    }
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

    // Predefined transform offsets (XZ plane; Y is 0 by default). We'll pick randomly among these.
    const std::vector<glm::vec3> predefinedOffsets = {
        glm::vec3(-2.0f, 0.0f, -1.0f),
        glm::vec3( 2.0f, 0.0f, -1.0f),
        glm::vec3(-2.0f, 0.0f,  1.0f),
        glm::vec3( 2.0f, 0.0f,  1.0f),
        glm::vec3( 0.0f, 0.0f,  2.5f),
        glm::vec3( 0.0f, 0.0f, -2.5f),
        glm::vec3( 1.5f, 0.0f,  0.0f),
        glm::vec3(-1.5f, 0.0f,  0.0f)
    };
    const float maxPlacementDistance = 10.0f; // clamp to this world-space distance from scene base offset

    // Modal popup state
    bool open_loading_all_modal = false;
    bool open_loading_scene_modal = false;
    std::string loading_scene_id;

    // --- Debug logging for loading UI ---
    const std::string log_file = "loading_ui_log.txt";
    std::vector<std::string> ui_logs;
    std::mutex ui_log_mtx;
    auto AppendLog = [&](const std::string& line){
        // timestamp
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%F %T") << "." << std::setw(3) << std::setfill('0') << ms << "  " << line;
        std::string out = ss.str();

        // append to file
        try {
            std::ofstream ofs(log_file, std::ios::app);
            if (ofs) ofs << out << "\n";
        } catch(...) {}

        // keep in-memory small buffer for UI
        {
            std::scoped_lock lk(ui_log_mtx);
            ui_logs.push_back(out);
            if (ui_logs.size() > 200) ui_logs.erase(ui_logs.begin(), ui_logs.begin() + (ui_logs.size() - 200));
        }
    };

    // tracking to avoid log floods
    float last_logged_pct_all = -1.0f;
    auto last_logged_all_time = std::chrono::high_resolution_clock::now();
    float last_logged_pct_scene = -1.0f;
    auto last_logged_scene_time = std::chrono::high_resolution_clock::now();

    // --- Fault test instrumentation ---
    struct Sample { double t; int64_t got; int64_t total; };
    bool faultTestRunning = false;
    double faultTestStartTime = 0.0;
    std::vector<Sample> faultSamples;
    float sampleIntervalSec = 0.5f; // default sampling interval (was double)
    float stallThresholdSec = 3.0f;  // consider stall if no progress for this many seconds (was double)

    std::unordered_map<std::string, int64_t> lastGotPerScene;
    std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> stallStartTime;
    std::unordered_map<std::string, bool> inStall;
    int totalStallEvents = 0;
    int totalRecoveryEvents = 0;
    double maxStallDuration = 0.0;

    auto ResetFaultTest = [&](){
        faultSamples.clear();
        lastGotPerScene.clear();
        stallStartTime.clear();
        inStall.clear();
        totalStallEvents = 0;
        totalRecoveryEvents = 0;
        maxStallDuration = 0.0;
        faultTestRunning = true;
        faultTestStartTime = std::chrono::duration<double>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        AppendLog("FaultTest: started");
    };

    // Timing/FPS
    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();
    double fps = 0.0;
    double frame_time_avg = 0.0;

    // Main loop
    AppendLog("App started");
    auto lastSampleTime = std::chrono::high_resolution_clock::now();
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
            AppendLog("View All pressed");
            // If all scenes already loaded, switch immediately.
            auto all_scenes_tmp = scheduler.GetAllScenes();
            bool all_loaded = true;
            for (auto &s : all_scenes_tmp) {
                if (s->state.load() != SceneState::LOADED) { all_loaded = false; break; }
            }
            AppendLog(std::string("All loaded? ") + (all_loaded ? "yes" : "no"));
            if (all_loaded) {
                view_mode = ViewMode::SHOW_ALL;
                view_scene_id.clear();
                AppendLog("Switching to SHOW_ALL immediately");
            } else {
                // open modal to show cumulative loading progress; prevent partial rendering
                open_loading_all_modal = true;
                view_mode = ViewMode::SHOW_NONE;
                view_scene_id.clear();
                AppendLog("Requested LoadingAllModal (deterministic window)");
                // reset progress logging trackers
                last_logged_pct_all = -1.0f;
                last_logged_all_time = clock::now();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide Models")) {
            view_mode = ViewMode::SHOW_NONE;
            view_scene_id.clear();
            AppendLog("Hide Models pressed -> SHOW_NONE");
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
                    AppendLog(std::string("Enqueued load for scene ") + sd->scene_id);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload")) {
                scheduler.UnloadScene(sd->scene_id);
                AppendLog(std::string("Unload requested for scene ") + sd->scene_id);
                // free GL resources
                std::scoped_lock lk(sd->mtx);
                for (auto &mh : sd->mesh_handles) {
                    renderer.DestroyMesh(mh);
                }
                sd->mesh_handles.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("View")) {
                AppendLog(std::string("View pressed for scene ") + sd->scene_id);
                // prioritize and, if loaded, set single view; otherwise open per-scene modal
                scheduler.PrioritizeScene(sd->scene_id);
                if (sd->state.load() == SceneState::LOADED) {
                    view_mode = ViewMode::SHOW_SINGLE;
                    view_scene_id = sd->scene_id;
                    AppendLog(std::string("Scene ") + sd->scene_id + " already loaded -> SHOW_SINGLE");

                    // compute base_offset and frame camera as before
                    auto all_tmp = scheduler.GetAllScenes();
                    int base_index = 0;
                    for (int bi = 0; bi < (int)all_tmp.size(); ++bi) {
                        if (all_tmp[bi]->scene_id == "scene05") { base_index = bi; break; }
                    }
                    glm::vec3 base_offset = glm::vec3(base_index * scene_spacing, 0.0f, 0.0f);

                    std::scoped_lock lk(sd->mtx);
                    if (!sd->model_bounds.empty()) {
                        int active = sd->current_model_index.load();
                        if (active < 0) active = 0;
                        if (active >= (int)sd->model_bounds.size()) active = (int)sd->model_bounds.size() - 1;
                        float radius = sd->model_bounds[active].radius;
                        radius = glm::max(radius, 0.5f);
                        camera.FrameBoundingSphere(base_offset, radius, (float)display_w / (float)display_h);
                    } else {
                        camera.SetTarget(base_offset);
                    }
                } else {
                    // open deterministic per-scene loading window and keep rendering hidden
                    open_loading_scene_modal = true;
                    loading_scene_id = sd->scene_id;
                    view_mode = ViewMode::SHOW_NONE;
                    AppendLog(std::string("Requested LoadingSceneModal (deterministic) for ") + loading_scene_id);
                    last_logged_pct_scene = -1.0f;
                    last_logged_scene_time = clock::now();
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

        // Deterministic centered windows for loading UI (guaranteed to show)
        if (open_loading_all_modal) {
            ImGui::SetNextWindowSize(ImVec2((float)display_w * 0.6f, 140.0f), ImGuiCond_Always);
            ImGui::SetNextWindowPos(ImVec2((float)display_w * 0.5f, (float)display_h * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
            ImGui::Begin("LoadingAllModal", nullptr, flags);
            // cumulative progress across all scenes
            int64_t total = 0, got = 0;
            auto all = scheduler.GetAllScenes();
            for (auto &s : all) {
                std::scoped_lock lk(s->mtx);
                for (auto &m : s->models) {
                    total += m.size_bytes;
                    got += m.bytes_received.load();
                }
            }
            float pct = (total > 0) ? (float)got / (float)total : 0.0f;
            ImGui::Text("Loading all scenes...");
            ImGui::Spacing();
            ImGui::ProgressBar(pct, ImVec2(-1, 40.0f));
            ImGui::Spacing();
            ImGui::Text("%.1f%%  ( %lld / %lld bytes )", pct * 100.0f, (long long)got, (long long)total);

            // log progress sparsely: every 1s or when pct changes by >=1%
            auto now_l = clock::now();
            double secs = std::chrono::duration<double>(now_l - last_logged_all_time).count();
            if (std::fabs(pct - last_logged_pct_all) >= 0.01f || secs >= 1.0) {
                AppendLog(std::string("LoadingAllModal progress: ") + std::to_string(pct*100.0f) + "% (" + std::to_string(got) + "/" + std::to_string(total) + ")");
                last_logged_pct_all = pct;
                last_logged_all_time = now_l;
            }

            ImGui::Separator();
            if (ImGui::Button("Cancel")) {
                open_loading_all_modal = false;
                AppendLog("LoadingAllModal: Cancel pressed");
            }
            ImGui::SameLine();
            if (ImGui::Button("Close && Show Whatever Loaded")) {
                open_loading_all_modal = false;
                view_mode = ViewMode::SHOW_ALL;
                AppendLog("LoadingAllModal: Closed manually -> switch to SHOW_ALL");
            }
            // auto-close and switch to view all when complete
            if (pct >= 0.999f) {
                open_loading_all_modal = false;
                view_mode = ViewMode::SHOW_ALL;
                view_scene_id.clear();
                AppendLog("LoadingAllModal: auto-complete -> SHOW_ALL");
            }
            ImGui::End();
        }

        if (open_loading_scene_modal) {
            ImGui::SetNextWindowSize(ImVec2((float)display_w * 0.5f, 140.0f), ImGuiCond_Always);
            ImGui::SetNextWindowPos(ImVec2((float)display_w * 0.5f, (float)display_h * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
            ImGui::Begin("LoadingSceneModal", nullptr, flags);
            int64_t total = 0, got = 0;
            auto all = scheduler.GetAllScenes();
            for (auto &s : all) {
                if (s->scene_id == loading_scene_id) {
                    std::scoped_lock lk(s->mtx);
                    for (auto &m : s->models) {
                        total += m.size_bytes;
                        got += m.bytes_received.load();
                    }
                    // if scene finished loading elsewhere, switch to it
                    if (s->state.load() == SceneState::LOADED) {
                        got = total;
                    }
                    break;
                }
            }
            float pct = (total > 0) ? (float)got / (float)total : 0.0f;
            ImGui::Text("Loading scene: %s", loading_scene_id.c_str());
            ImGui::Spacing();
            ImGui::ProgressBar(pct, ImVec2(-1, 40.0f));
            ImGui::Spacing();
            ImGui::Text("%.1f%%  ( %lld / %lld bytes )", pct * 100.0f, (long long)got, (long long)total);

            // log progress sparsely: every 1s or when pct changes by >=1%
            auto now_s = clock::now();
            double secs_s = std::chrono::duration<double>(now_s - last_logged_scene_time).count();
            if (std::fabs(pct - last_logged_pct_scene) >= 0.01f || secs_s >= 1.0) {
                AppendLog(std::string("LoadingSceneModal(") + loading_scene_id + ") progress: " + std::to_string(pct*100.0f) + "% (" + std::to_string(got) + "/" + std::to_string(total) + ")");
                last_logged_pct_scene = pct;
                last_logged_scene_time = now_s;
            }

            ImGui::Separator();
            if (ImGui::Button("Cancel")) {
                open_loading_scene_modal = false;
                AppendLog(std::string("LoadingSceneModal(") + loading_scene_id + "): Cancel pressed");
            }
            ImGui::SameLine();
            if (ImGui::Button("Close & Show Loaded Model")) {
                open_loading_scene_modal = false;
                view_mode = ViewMode::SHOW_SINGLE;
                view_scene_id = loading_scene_id;
                AppendLog(std::string("LoadingSceneModal(") + loading_scene_id + "): closed manually -> SHOW_SINGLE");
            }
            if (pct >= 0.999f) {
                open_loading_scene_modal = false;
                view_mode = ViewMode::SHOW_SINGLE;
                view_scene_id = loading_scene_id;
                AppendLog(std::string("LoadingSceneModal(") + loading_scene_id + "): auto-complete -> SHOW_SINGLE");
            }
            ImGui::End();
        }

        // --- Fault test sampling (runs async within main loop) ---
        if (faultTestRunning) {
            auto now_samp = std::chrono::high_resolution_clock::now();
            double elapsedSinceLastSample = std::chrono::duration<double>(now_samp - lastSampleTime).count();
            if (elapsedSinceLastSample >= sampleIntervalSec) {
                lastSampleTime = now_samp;
                // compute cumulative progress and per-scene got totals
                int64_t total = 0, got = 0;
                auto all = scheduler.GetAllScenes();
                for (auto &s : all) {
                    int64_t sceneGot = 0;
                    std::scoped_lock lk(s->mtx);
                    for (auto &m : s->models) {
                        total += m.size_bytes;
                        sceneGot += m.bytes_received.load();
                    }
                    got += sceneGot;
                    // stall detection per scene
                    auto it = lastGotPerScene.find(s->scene_id);
                    if (it == lastGotPerScene.end()) {
                        lastGotPerScene[s->scene_id] = sceneGot;
                        inStall[s->scene_id] = false;
                    } else {
                        if (sceneGot == it->second) {
                            // no progress since last sample
                            // only consider stall if scene is LOADING or QUEUED
                            if ((s->state.load() == SceneState::LOADING || s->state.load() == SceneState::QUEUED) && !inStall[s->scene_id]) {
                                // start stall
                                stallStartTime[s->scene_id] = now_samp;
                                inStall[s->scene_id] = true;
                                totalStallEvents++;
                                AppendLog(std::string("FaultTest: stall detected for scene ") + s->scene_id);
                            }
                        } else {
                            // progress occurred
                            if (inStall[s->scene_id]) {
                                // recovery
                                auto start = stallStartTime[s->scene_id];
                                double dur = std::chrono::duration<double>(now_samp - start).count();
                                totalRecoveryEvents++;
                                if (dur > maxStallDuration) maxStallDuration = dur;
                                AppendLog(std::string("FaultTest: recovery for scene ") + s->scene_id + " after " + std::to_string(dur) + "s");
                                inStall[s->scene_id] = false;
                            }
                            lastGotPerScene[s->scene_id] = sceneGot;
                        }
                    }
                }

                double tNow = std::chrono::duration<double>(now_samp.time_since_epoch()).count();
                faultSamples.push_back({ tNow - faultTestStartTime, got, total });

                // if everything loaded, stop test and report summary
                bool allLoaded = true;
                for (auto &s : all) {
                    if (s->state.load() != SceneState::LOADED) { allLoaded = false; break; }
                }
                if (allLoaded) {
                    faultTestRunning = false;
                    AppendLog("FaultTest: completed - all scenes loaded");
                    // compute throughput and duration
                    double duration = 0.0;
                    if (!faultSamples.empty()) {
                        duration = faultSamples.back().t;
                    }
                    // compute average throughput bytes/sec
                    double avgThroughput = 0.0;
                    if (duration > 0.0 && !faultSamples.empty()) {
                        avgThroughput = (double)faultSamples.back().got / duration;
                    }
                    std::ostringstream ss;
                    ss << "FaultTest summary: duration=" << duration << "s, stalls=" << totalStallEvents << ", recoveries=" << totalRecoveryEvents << ", maxStall=" << maxStallDuration << "s, avgThroughput=" << avgThroughput << " B/s";
                    AppendLog(ss.str());
                }
            }
        }

        // Small on-screen log window for quick feedback + Fault Test UI
        ImGui::Begin("Loading UI Log");
        {
            std::scoped_lock lk(ui_log_mtx);
            int start = (ui_logs.size() > 12) ? (int)ui_logs.size() - 12 : 0;
            for (int i = start; i < (int)ui_logs.size(); ++i) {
                ImGui::TextWrapped("%s", ui_logs[i].c_str());
            }
        }

        // Fault Test controls and summary
        ImGui::Separator();
        ImGui::Text("Fault Test (empirical proof)");
        if (!faultTestRunning) {
            if (ImGui::Button("Start Fault Test")) {
                ResetFaultTest();
            }
            ImGui::SameLine();
            if (ImGui::Button("Export Last Results")) {
                if (!faultSamples.empty()) {
                    std::ofstream ofs("fault_test_results.txt", std::ios::trunc);
                    if (ofs) {
                        ofs << "Fault Test Results\n";
                        ofs << "time_s\tbytes_received\ttotal_bytes\n";
                        for (auto &s : faultSamples) {
                            ofs << s.t << "\t" << s.got << "\t" << s.total << "\n";
                        }
                        ofs.close();
                        AppendLog("FaultTest: exported fault_test_results.txt");
                    } else {
                        AppendLog("FaultTest: failed to open fault_test_results.txt for writing");
                    }
                } else {
                    AppendLog("FaultTest: no samples to export");
                }
            }
            ImGui::SliderFloat("Sample interval (s)", &sampleIntervalSec, 0.1f, 5.0f);
            ImGui::SliderFloat("Stall threshold (s)", &stallThresholdSec, 0.5f, 20.0f);
        } else {
            if (ImGui::Button("Stop Fault Test")) {
                faultTestRunning = false;
                AppendLog("FaultTest: stopped by user");
            }
            ImGui::SameLine();
            if (ImGui::Button("Export Partial Results")) {
                if (!faultSamples.empty()) {
                    std::ofstream ofs("fault_test_results_partial.txt", std::ios::trunc);
                    if (ofs) {
                        ofs << "Fault Test Partial Results\n";
                        ofs << "time_s\tbytes_received\ttotal_bytes\n";
                        for (auto &s : faultSamples) {
                            ofs << s.t << "\t" << s.got << "\t" << s.total << "\n";
                        }
                        ofs.close();
                        AppendLog("FaultTest: exported fault_test_results_partial.txt");
                    } else {
                        AppendLog("FaultTest: failed to open fault_test_results_partial.txt for writing");
                    }
                } else {
                    AppendLog("FaultTest: no samples to export");
                }
            }
        }

        // show quick summary
        ImGui::Text("Samples: %zu", faultSamples.size());
        ImGui::Text("Stalls: %d  Recoveries: %d  MaxStall(s): %.2f", totalStallEvents, totalRecoveryEvents, maxStallDuration);

        // small progress plot (percentage)
        if (!faultSamples.empty()) {
            static std::vector<float> plotData;
            plotData.clear();
            plotData.reserve(faultSamples.size());
            for (auto &s : faultSamples) {
                float pct = (s.total > 0) ? (float)s.got / (float)s.total : 0.0f;
                plotData.push_back(pct * 100.0f);
            }
            ImGui::PlotLines("Cumulative %", plotData.data(), (int)plotData.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(0,80));
        }

        if (ImGui::Button("Clear Log")) {
            std::scoped_lock lk(ui_log_mtx);
            ui_logs.clear();
            // truncate file
            std::ofstream ofs(log_file, std::ios::trunc);
            (void)ofs;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit")) {
            AppendLog("Quit requested via UI");
            // Signal the main loop to exit so the graceful shutdown sequence runs
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

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

        // Render skybox first (so it sits behind everything)
        renderer.RenderSkybox(view, proj);

        // Render a flat ground plane under models
        renderer.RenderPlane(viewProj, glm::vec3(0.35f, 0.35f, 0.38f));

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

                glm::mat4 modelLocal = (sd->model_transforms.size() > (size_t)active) ? sd->model_transforms[active] : glm::mat4(1.0f);

                // choose base offset per scene (for grouping)
                glm::vec3 scene_base = glm::vec3(scene_index * scene_spacing, 0.0f, 0.0f);

                // For each model compute a deterministic "random" placement selected from predefinedOffsets.
                // Seed uses scene_id and model index so placement is stable across frames/runs.
                size_t seed = std::hash<std::string>{}(sd->scene_id) ^ (static_cast<size_t>(active) * 0x9e3779b97f4a7c15ULL);
                std::mt19937_64 rng(seed);
                std::uniform_int_distribution<int> dist(0, static_cast<int>(predefinedOffsets.size()) - 1);
                glm::vec3 chosen = predefinedOffsets[dist(rng)];

                // Optionally add small jitter (clamped)
                std::uniform_real_distribution<float> jitterDist(-0.25f, 0.25f);
                float jx = jitterDist(rng);
                float jz = jitterDist(rng);
                chosen += glm::vec3(jx, 0.0f, jz);

                // Clamp distance from scene_base
                glm::vec3 worldPos = scene_base + chosen;
                float distLen = glm::length(glm::vec3(worldPos.x - scene_base.x, 0.0f, worldPos.z - scene_base.z));
                if (distLen > maxPlacementDistance) {
                    glm::vec3 dir = glm::normalize(glm::vec3(worldPos.x - scene_base.x, 0.0f, worldPos.z - scene_base.z));
                    worldPos = scene_base + dir * maxPlacementDistance;
                }

                // Final model matrix: translate by worldPos then apply local model transform
                glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos) * modelLocal;

                // keep vertical alignment if necessary (preserve Y)
                model[3].y = worldPos.y;

                renderer.RenderMesh(mh, model, viewProj, glm::vec3(0.8f, 0.8f, 0.9f));
                ++scene_index;
            }
        }

        // Render ImGui on top
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    AppendLog("App exiting - initiating graceful shutdown");

    // 1) Stop scheduler so it won't enqueue new work or request new downloads.
    try {
        scheduler.Stop();
        AppendLog("Scheduler stopped");
    } catch (const std::exception& ex) {
        AppendLog(std::string("Exception while stopping scheduler: ") + ex.what());
    } catch (...) {
        AppendLog("Unknown exception while stopping scheduler");
    }

    // 2) Notify any threads waiting on the upload_cv so they can wake and finish.
    upload_cv.notify_all();
    AppendLog("Notified upload_cv to wake any waiting threads");

    // 3) Drain remaining GL upload tasks on the main thread before shutting down the loader/renderer.
    //    This ensures no background thread will attempt GL calls after we tear down the GL context.
    {
        auto drain_start = clock::now();
        while (true) {
            bool didWork = false;
            {
                std::scoped_lock lk(upload_mtx);
                while (!upload_queue.empty()) {
                    didWork = true;
                    try {
                        auto task = std::move(upload_queue.front());
                        upload_queue.pop();
                        task(); // perform GL upload on main thread
                    } catch (const std::exception& ex) {
                        AppendLog(std::string("Exception in upload task: ") + ex.what());
                    } catch (...) {
                        AppendLog("Unknown exception executing upload task");
                    }
                }
            }
            if (!didWork) break;
            // Give other threads a moment to push final tasks (bounded wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (std::chrono::duration<double>(clock::now() - drain_start).count() > 5.0) {
                AppendLog("Timed out while draining upload_queue (5s)");
                break;
            }
        }
        AppendLog("Upload queue drained (or timed out)");
    }

    // 4) Shutdown loader (stop its background threads, close file handles, etc.)
    try {
        loader.Shutdown();
        AppendLog("Loader shutdown complete");
    } catch (const std::exception& ex) {
        AppendLog(std::string("Exception while shutting down loader: ") + ex.what());
    } catch (...) {
        AppendLog("Unknown exception while shutting down loader");
    }

    // 5) Free remaining GL resources created from scenes now that loader is stopped and uploads are drained.
    try {
        auto all_scenes = scheduler.GetAllScenes();
        for (auto &sd : all_scenes) {
            std::scoped_lock lk(sd->mtx);
            for (auto &mh : sd->mesh_handles) {
                renderer.DestroyMesh(mh);
            }
            sd->mesh_handles.clear();
        }
        AppendLog("Destroyed scene mesh handles");
    } catch (const std::exception& ex) {
        AppendLog(std::string("Exception while destroying meshes: ") + ex.what());
    } catch (...) {
        AppendLog("Unknown exception while destroying meshes");
    }

    // 6) Final renderer/ImGui/GLFW teardown (must occur after GL resources freed and loader fully stopped).
    try {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        AppendLog("ImGui shutdown complete");
    } catch (const std::exception& ex) {
        AppendLog(std::string("Exception during ImGui shutdown: ") + ex.what());
    } catch (...) {
        AppendLog("Unknown exception during ImGui shutdown");
    }

    // Destroy window and terminate GLFW last
    try {
        glfwDestroyWindow(window);
        glfwTerminate();
        AppendLog("GLFW terminated");
    } catch (const std::exception& ex) {
        AppendLog(std::string("Exception during GLFW shutdown: ") + ex.what());
    } catch (...) {
        AppendLog("Unknown exception during GLFW shutdown");
    }

    AppendLog("Shutdown complete, exiting normally");
    return 0;
}